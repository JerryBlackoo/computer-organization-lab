#include "../simulation/systemc/platform.hpp"
#include <iostream>

using namespace colab;
namespace sc = simulation::systemc;
namespace common = devices::common;
static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static unsigned leaf(unsigned pa, unsigned flags = 0xc7) { return ((pa >> 12) << 10) | flags; }
static unsigned load(unsigned rd, unsigned rs) { return (rs << 15) | (rd << 7) | 0x2003; }
static unsigned fence(unsigned rs1, unsigned rs2) { return 0x12000073 | (rs1 << 15) | (rs2 << 20); }
static sc::BootImage image() {
    sc::BootImage boot{0x8000, {0x30200073}, {}};
    const unsigned code[]{
        load(10, 1), load(11, 2), load(12, 6), load(13, 3), load(14, 4), load(15, 8), load(16, 9),
        fence(1, 5), load(17, 1), load(18, 2), load(19, 6),
        fence(8, 5), load(20, 8), load(21, 9),
        fence(0, 5), load(22, 2), load(23, 6),
        (0x180u << 20) | (7u << 15) | 0x1073u, // CSRRW x0,satp,x7: change ASID, no implicit flush.
        load(24, 6), load(25, 2), fence(6, 0), load(26, 6), fence(0, 0), 0x10500073
    };
    for (unsigned i = 0; i < std::size(code); ++i) boot.data.push_back({0x9000 + 4 * i, code[i]});
    const std::pair<unsigned, unsigned> data[]{
        {0x1400, 0x801}, {0x1000, 0xc01}, {0x1800, leaf(0, 0xcf)},
        {0x2024, leaf(0x9000, 0x4b)}, {0x2030, leaf(0xc000)}, {0x2034, leaf(0xd000)},
        {0x2038, leaf(0x11000, 0xe7)}, {0x3000, leaf(0x14000)}, {0x3008, leaf(0x15000)},
        {0xc000, 41}, {0xd000, 42}, {0x11000, 51}, {0xa000, 73}, {0x1a000, 74},
        {0x14000, 61}, {0x15000, 62}, {0xf000, 99}, {0x12000, 88}, {0x13000, 52},
        {0x16000, 63}, {0x17000, 64}
    };
    boot.data.insert(boot.data.end(), std::begin(data), std::end(data));
    return boot;
}
struct Fixture {
    sc::Platform& machine;
    soc::core::mmu::TlbConfig config;
    template<class F> void until(F predicate) {
        for (unsigned n = 0; n < 20000 && !predicate(); ++n) {
            machine.step();
            require(machine.fault().empty() && !machine.cpu().interrupts().exceptions(), "configured CPU faulted during paging");
        }
        require(predicate(), "configured platform timed out");
    }
    void retired(unsigned count) { until([&] { return machine.cpu().completed_instructions() == count; }); }
    void check_reset() {
        const auto& cpu = machine.cpu();
        require(cpu.sv32().geometry().capacity() == config.entries && cpu.sv32().geometry().ways() == config.ways &&
            machine.cpu().mmu().tlb().geometry().capacity() == config.entries &&
            machine.cpu().mmu().tlb().geometry().ways() == config.ways, "reset lost configured geometry in either TLB");
        require(!cpu.sv32().busy() && !cpu.sv32().statistics().walks && !cpu.completed_instructions(), "reset retained walk/statistics/retirement");
        for (const auto& entry : cpu.sv32().entries()) require(!entry.valid, "reset retained an architectural TLB entry");
        for (const auto& entry : machine.cpu().mmu().tlb().entries()) require(!entry.valid, "reset retained a teaching TLB entry");
    }
    void prepare() {
        auto& cpu = machine.cpu(); auto& state = cpu.interrupts();
        state.write(0x3b0, 0x1fffffff); state.write(0x3a0, 0x1f);
        state.write(0x180, 0x80000001); state.write(0x341, 0x40009000); state.write(0x300, 1u << 11);
        cpu.set_register(1, 0x4000c000); cpu.set_register(2, 0x4000d000); cpu.set_register(3, 0x8000a000);
        cpu.set_register(4, 0x8001a000); cpu.set_register(5, 0x200); // Non-x0 with implemented ASID bits zero.
        cpu.set_register(6, 0x4000e000); cpu.set_register(7, 0x80400001); cpu.set_register(8, 0); cpu.set_register(9, 0x2000);
        cpu.mmu().enable_paging(0, [](unsigned) -> soc::core::mmu::PageTableEntry {
            throw std::runtime_error("formal CPU called the teaching MMU");
        });
    }
    void execute() {
        prepare(); retired(5); // MRET + A/B/global/first superpage load.
        const auto before = machine.cpu().sv32().statistics();
        retired(6); // Same superpage, different 4KiB subpage: no additional walk or PTE read.
        require(machine.cpu().sv32().statistics().walks == before.walks &&
            machine.cpu().sv32().statistics().reads == before.reads && machine.cpu().registers().read(14) == 74,
            "real CPU superpage subpage access missed the TLB");
        retired(8);
        const unsigned expected[]{41, 42, 51, 73, 74, 61, 62};
        for (unsigned i = 0; i < std::size(expected); ++i)
            require(machine.cpu().registers().read(10 + i) == expected[i], "initial guest paged data differed");
        for (const auto& item : {std::pair<unsigned, unsigned>{0x2030, leaf(0xf000)}, {0x2034, leaf(0x12000)},
            {0x2038, leaf(0x13000, 0xe7)}, {0x3000, leaf(0x16000)}, {0x3008, leaf(0x17000)}})
            machine.memory().write(item.first, common::word_bytes(item.second));
        until([&] { return machine.cpu().waiting(); });
        const unsigned final[]{99, 42, 51, 63, 62, 88, 51, 51, 88, 52};
        for (unsigned i = 0; i < std::size(final); ++i)
            require(machine.cpu().registers().read(17 + i) == final[i], "guest selective SFENCE/ASID/global result differed");
        require(machine.cpu().completed_instructions() == 25 && machine.cpu().sv32().statistics().flushes == 5 &&
            machine.cpu().interrupts().read(0x180) == 0x80400001 && machine.cpu().mmu().statistics().page_walks == 0,
            "guest SFENCE retirement/statistics/translation isolation differed");
        require(machine.instruction_cache().cache().statistics().fills && machine.data_cache().cache().statistics().fills &&
            machine.cpu().bus_waits(), "configured paging bypassed real Cache/TLM/DIMM waits");
    }
};
int sc_main(int argc, char** argv) {
    try {
        SoCConfig config;
        if (argc > 1) config.tlb.ways = std::stoul(argv[1]);
        if (argc > 2) config.tlb.entries = std::stoul(argv[2]);
        sc::Platform machine("configured_tlb", image(), sc_core::sc_time(50, sc_core::SC_US), config);
        Fixture f{machine, config.tlb};
        machine.power_on(); f.check_reset(); f.execute();
        machine.reset(); f.check_reset(); f.prepare();
        f.until([&] { return machine.cpu().sv32().busy(); });
        machine.pause(); const auto before = machine.cpu().sv32().statistics().reads;
        sc_core::sc_start(sc_core::sc_time(1, sc_core::SC_MS));
        require(machine.cpu().sv32().statistics().reads == before, "paused configured walker advanced");
        machine.reset(); f.check_reset(); f.prepare(); f.retired(2);
        require(machine.cpu().registers().read(10) == 99, "warm reset lost RAM or consumed a stale PTE response");
        machine.reset(); f.prepare(); f.until([&] { return machine.cpu().sv32().busy(); });
        machine.power_off(); sc_core::sc_start(sc_core::sc_time(2, sc_core::SC_MS)); f.check_reset();
        machine.power_on(); f.check_reset(); f.execute();
        std::cout << "PASS configured SystemC TLB: " << config.tlb.entries << " entries / " << config.tlb.ways
                  << " ways; real 4K/4M CPU loads, selective guest SFENCE, ASID/global, pause/reset/off/cold reload\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL configured TLB: " << error.what() << '\n'; return 1; }
}
