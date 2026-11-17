#include "../soc/core/rv32_core.hpp"
#include <iostream>

using namespace colab::soc::core;
using Access = mmu::AccessType;
using Memory = datapath::dedicated::DataMemory;
static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static unsigned leaf(unsigned pa, unsigned flags = 0xcf) { return ((pa >> 12) << 10) | flags; }
static constexpr unsigned Base = 0x40000000, Super = 0x80000000;
struct Fixture {
    Memory memory{16384};
    InterruptSystem state;
    Sv32 tlb;
    explicit Fixture(mmu::TlbConfig config = {}) : tlb(config) {
        state.write(0x3b0, 0x1fffffff); state.write(0x3a0, 0x1f);
        state.write(0x180, 0x80000001); state.write(0x300, 1u << 11); state.mret();
        memory.write(0x1400, 0x801); // Root[256] -> table at 0x2000.
        memory.write(0x1000, 0xc01); memory.write(0x3000, leaf(0xa000));
        memory.write(0x1800, leaf(0)); // Root[512]: 4MiB leaf.
        for (unsigned i = 0; i < 1024; ++i) memory.write(0x2000 + 4 * i, leaf(0x10000 + i * 4096));
    }
    void asid(unsigned value) { state.write(0x180, 0x80000001 | ((value & 511) << 22)); }
    unsigned translate(unsigned va, Access access = Access::Read) {
        const auto pa = tlb.translate(va, 4, access, 0x9000, state, nullptr, memory);
        require(pa.has_value(), "local Sv32 walk did not finish"); return *pa;
    }
    void probe(unsigned va, unsigned pa, bool hit, unsigned id = 0) {
        asid(id); const auto before = tlb.statistics();
        require(translate(va) == pa, "Sv32 translation/PFN/tag mismatch");
        require(tlb.statistics().walks == before.walks + !hit && tlb.statistics().hits == before.hits + hit,
            "Sv32 hit/miss behavior disagrees with expected residency");
    }
};
static void ordinary(mmu::TlbConfig config) {
    Fixture f(config);
    const auto sets = f.tlb.geometry().sets(), ways = config.ways;
    if (sets > 1) f.probe(Base + 4096, 0x11000, false);
    for (unsigned i = 0; i < ways; ++i) f.probe(Base + static_cast<unsigned>(i * sets * 4096),
        0x10000 + static_cast<unsigned>(i * sets * 4096), false);
    f.probe(Base, 0x10000, true); // Hits must NOT move the Sv32 round-robin cursor.
    f.probe(Base + static_cast<unsigned>(ways * sets * 4096), 0x10000 + static_cast<unsigned>(ways * sets * 4096), false);
    if (sets > 1) f.probe(Base + 4096, 0x11000, true);
    f.probe(Base, 0x10000, false); // Way 0 was evicted despite its recent hit.
    for (std::size_t i = 0; i < f.tlb.entries().size(); ++i) {
        const auto& entry = f.tlb.entries()[i];
        require(!entry.valid || f.tlb.geometry().set_index(entry.vpn >> (10 * entry.level)) == i / ways,
            "Sv32 replacement crossed a set boundary");
    }
}
static void superpages(mmu::TlbConfig config) {
    Fixture f(config);
    f.probe(Super + 0x1000, 0x1000, false);
    for (unsigned offset : {0u, 0x2000u, 0x3ff000u, 0x2aa004u}) f.probe(Super + offset, offset, true);
    require(f.tlb.statistics().reads == 1, "superpage subpages performed extra page-table reads");
    const auto sets = f.tlb.geometry().sets();
    const auto page = sets > 1 ? 1u : 0u; // Keep both sizes resident, also for direct mapping.
    f.probe(Base + page * 4096, 0x10000 + page * 4096, false);
    for (unsigned n = 0; n < 16; ++n) {
        f.probe(Super + (n * 17 + 3) * 4096, (n * 17 + 3) * 4096, true);
        f.probe(Base + page * 4096, 0x10000 + page * 4096, true);
    }
    Fixture shared(config);
    shared.probe(Super, 0, false); // Superpage and ordinary pages really share the same ways.
    for (unsigned i = 0; i < config.ways; ++i)
        shared.probe(Base + static_cast<unsigned>(i * sets * 4096), 0x10000 + static_cast<unsigned>(i * sets * 4096), false);
    shared.probe(Super + 0x123000, 0x123000, false);
}
static void asids_and_permissions(mmu::TlbConfig config) {
    Fixture f(config);
    f.memory.write(0x2000, leaf(0x5000)); f.probe(Base, 0x5000, false);
    f.memory.write(0x2000, leaf(0x6000)); f.probe(Base, 0x6000, false, 1);
    f.probe(Base, config.ways > 1 ? 0x5000 : 0x6000, config.ways > 1);
    const auto before = f.tlb.statistics().walks;
    f.state.write(0x180, 0); require(f.translate(Base) == Base, "Bare translation stopped bypassing TLB");
    f.asid(0); require(f.translate(Base) == (config.ways > 1 ? 0x5000u : 0x6000u) &&
        f.tlb.statistics().walks == before, "satp implicitly flushed TLB");
    f.tlb.flush(); f.memory.write(0x1400, 0x821); // Inherited global, not just a leaf G bit.
    f.probe(Base, 0x6000, false); f.probe(Base + 4, 0x6004, true, 511);
    f.state.write(0x300, InterruptSystem::MPRV | (1u << 11)); // Still S, SUM disabled.
    f.tlb.flush(); f.memory.write(0x2000, leaf(0x7000, 0x59)); // User execute-only, A=1,D=0.
    f.state.write(0x100, InterruptSystem::SUM | InterruptSystem::MXR);
    f.probe(Base, 0x7000, false); f.probe(Base, 0x7000, true);
    f.state.write(0x100, InterruptSystem::SUM);
    try { f.translate(Base); throw std::runtime_error("cached MXR permission was not rechecked"); }
    catch (const RV32Fault& error) { require(error.cause() == RV32FaultCause::LoadPageFault, "wrong permission fault"); }
    f.state.write(0x100, InterruptSystem::SUM | InterruptSystem::MXR);
    try { f.translate(Base, Access::Write); throw std::runtime_error("cached write permission/Svade was not rechecked"); }
    catch (const RV32Fault& error) { require(error.cause() == RV32FaultCause::StorePageFault, "wrong store fault"); }
    require(f.memory.read(0x2000) == leaf(0x7000, 0x59), "fault modified guest PTE");
    f.tlb.flush(); f.memory.write(0x2000, leaf(0x7000, 0x47)); // S RW, A=1 but D=0.
    f.probe(Base, 0x7000, false);
    try { f.translate(Base, Access::Write); throw std::runtime_error("cached D=0 store did not fault"); }
    catch (const RV32Fault& error) { require(error.cause() == RV32FaultCause::StorePageFault, "wrong Svade fault"); }
    require(f.memory.read(0x2000) == leaf(0x7000, 0x47), "Svade updated the PTE instead of faulting");
}
static void overlapping_fences(mmu::TlbConfig config) {
    Fixture f(config);
    f.probe(Base + 0x1000, 0x11000, false);
    f.memory.write(0x1400, leaf(0)); // Leave an old 4KiB entry resident while installing a superpage.
    f.probe(Base + 0x2000, 0x2000, false);
    f.memory.write(0x1400, 0x801); f.memory.write(0x2004, leaf(0x6000));
    f.tlb.flush(Base + 0x1004); // Must remove BOTH page levels that cover this address.
    f.probe(Base + 0x1000, 0x6000, false); f.probe(Base + 0x2000, 0x12000, false);
}
static Fixture fence_fixture() {
    Fixture f({64, 4});
    f.memory.write(0x2004, leaf(0x5000)); f.memory.write(0x2008, leaf(0x6000));
    f.memory.write(0x200c, leaf(0x7000, 0xef)); f.memory.write(0x1804, leaf(0x400000, 0xef));
    for (unsigned id : {0u, 1u}) {
        f.probe(Base + 0x1000, 0x5000, false, id); f.probe(Base + 0x2000, 0x6000, false, id);
        f.probe(Super, 0, false, id);
    }
    f.probe(Base + 0x3000, 0x7000, false); f.probe(Super + 0x400000, 0x400000, false);
    return f;
}
static void fences() {
    {
        auto f = fence_fixture(); f.tlb.flush(Base + 0x1004);
        f.probe(Base + 0x1000, 0x5000, false); f.probe(Base + 0x1000, 0x5000, false, 1);
        f.probe(Base + 0x2000, 0x6000, true); f.probe(Super + 0x400000, 0x400000, true);
    }
    {
        auto f = fence_fixture(); f.tlb.flush(std::nullopt, 1);
        f.probe(Base + 0x1000, 0x5000, true); f.probe(Base + 0x1000, 0x5000, false, 1);
        f.probe(Base + 0x2000, 0x6000, false, 1); f.probe(Super + 0x2000, 0x2000, false, 1);
        f.probe(Base + 0x3000, 0x7000, true, 1); f.probe(Super + 0x400000, 0x400000, true, 1);
    }
    {
        auto f = fence_fixture(); f.tlb.flush(Super + 0x3ff004, 0); // Interior address invalidates whole leaf.
        f.probe(Super, 0, true, 1); f.probe(Super + 0x1000, 0x1000, false);
        f.probe(Base + 0x2000, 0x6000, true);
        f.tlb.flush(Super + 0x401000, 0); f.probe(Super + 0x400000, 0x400000, true);
        f.tlb.flush(Super + 0x401000); f.probe(Super + 0x400000, 0x400000, false);
    }
    {
        auto f = fence_fixture(); f.probe(0, 0xa000, false);
        f.tlb.flush(0, 0x200); // Explicit zero VA, explicit ASID zero after masking.
        f.probe(0, 0xa000, false); f.probe(Base + 0x1000, 0x5000, true);
        f.tlb.flush();
        for (const auto& entry : f.tlb.entries()) require(!entry.valid, "full fence retained a global/ASID/leaf entry");
        f.probe(Base + 0x3000, 0x7000, false);
        require(f.tlb.statistics().flushes == 2, "flush statistics changed meaning");
    }
}
// Immediate, byte-accurate bus for testing both CPU controllers; Platform tests
// separately exercise the delayed Cache/TLM/DIMM path.
class CpuBus : public colab::devices::common::DmaBus {
public:
    Memory ram{16384};
    colab::devices::common::Tag submit(colab::devices::common::Master master,
        colab::devices::common::BusOperation operation, std::uint64_t address, std::size_t bytes,
        colab::devices::common::Bytes data = {}) override {
        if (result_) throw std::length_error("CPU test bus busy");
        require(master == 0 && bytes == 4 && is_memory(static_cast<unsigned>(address), bytes) && address < 65536,
            "invalid CPU test transaction");
        using namespace colab::devices;
        if (operation == common::BusOperation::Write) ram.write(address, common::load32(data));
        result_ = common::BusResult{++tag_, master, colab::interconnect::pcie::Status::Success,
            operation == common::BusOperation::Read ? common::word_bytes(ram.read(address)) : common::Bytes{}, 4, {}, false};
        return tag_;
    }
    bool ready(colab::devices::common::Tag tag) const override { return result_ && tag == tag_; }
    colab::devices::common::BusResult take(colab::devices::common::Tag tag) override {
        require(ready(tag), "CPU took a missing response"); auto result = *result_; result_.reset(); return result;
    }
    bool is_memory(std::uint32_t address, std::size_t size) const override { return size && std::uint64_t(address) + size <= 65536; }
private:
    std::optional<colab::devices::common::BusResult> result_;
    colab::devices::common::Tag tag_ = 0;
};
static void cpu_fences() {
    using CPU = colab::soc::RV32Core;
    for (auto mode : {CPU::Mode::Hardwired, CPU::Mode::Microprogrammed}) {
        CpuBus bus;
        bus.ram.write(0x8000, 0x30200073);
        const unsigned code[]{0x0000a503, 0x00012583, 0x12308073, 0x0000a603, 0x00012683,
            0x12300073, 0x00012703, 0x12010073, 0x00012783, 0x10500073};
        for (unsigned i = 0; i < std::size(code); ++i) bus.ram.write(0x9000 + 4 * i, code[i]);
        bus.ram.write(0x1400, 0x801); bus.ram.write(0x2024, leaf(0x9000, 0x4b));
        bus.ram.write(0x2030, leaf(0xc000)); bus.ram.write(0x2034, leaf(0xd000, 0xef));
        bus.ram.write(0xc000, 41); bus.ram.write(0xd000, 42); bus.ram.write(0xe000, 99); bus.ram.write(0xf000, 88);
        CPU cpu({}, mode, 256, {}, CPU::Execution::MultiCycle, {}, FaultHandling::MachineTrap,
            CPU::Translation::Architectural, {8, 2});
        cpu.connect_bus(bus); cpu.connect_instruction_bus(bus); cpu.set_boot_pc(0x8000);
        cpu.set_register(1, Base + 0xc000); cpu.set_register(2, Base + 0xd000); cpu.set_register(3, 0);
        auto& state = cpu.interrupts(); state.write(0x3b0, 0x1fffffff); state.write(0x3a0, 0x1f);
        state.write(0x180, 0x80000001); state.write(0x341, Base + 0x9000); state.write(0x300, 1u << 11);
        for (unsigned n = 0; n < 1000 && cpu.completed_instructions() < 3; ++n) cpu.tick();
        require(cpu.completed_instructions() == 3, "controller did not finish initial paged loads");
        bus.ram.write(0x2030, leaf(0xe000)); bus.ram.write(0x2034, leaf(0xf000, 0xef));
        for (unsigned n = 0; n < 2000 && !cpu.waiting() && !state.exceptions(); ++n) cpu.tick();
        const unsigned expected[]{41, 42, 99, 42, 42, 88};
        for (unsigned i = 0; i < std::size(expected); ++i)
            require(cpu.registers().read(10 + i) == expected[i], "controller lost SFENCE operand/global semantics");
        require(cpu.waiting() && !state.exceptions() && cpu.completed_instructions() == 11 &&
            cpu.sv32().statistics().flushes == 3, "controller selective SFENCE did not retire precisely");
    }
}
int main() {
    Sv32 defaults;
    require(defaults.entries().size() == 16 && defaults.geometry().ways() == 16, "Sv32 default changed");
    for (std::size_t ways : {1u, 2u, 4u, 16u}) {
        const mmu::TlbConfig config{16, ways};
        ordinary(config); superpages(config); asids_and_permissions(config); overlapping_fences(config);
    }
    Fixture large({2048, 1}); large.probe(Super + 0x2aa000, 0x2aa000, false);
    large.probe(Super + 0x3ff000, 0x3ff000, true);
    require(large.tlb.entries()[512].valid && large.tlb.entries()[512].level == 1,
        "superpage indexing incorrectly retained 4KiB subpage bits for large set counts");
    fences(); cpu_fences();
    std::cout << "PASS Sv32 TLB: 1/2/4/FA RR/isolation, shared 4K/4M ways, subpage hits, ASID/global, permissions, selective fences\n";
}
