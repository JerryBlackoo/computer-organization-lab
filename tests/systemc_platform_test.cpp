#include "../simulation/systemc/platform.hpp"
#include "../examples/guest/io_driver.hpp"
#include "elf_fixture.hpp"
#include <iostream>
#include <set>

namespace sc = colab::simulation::systemc;
namespace common = colab::devices::common;
using namespace colab;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static common::Bytes pattern(std::size_t size = 512) {
    common::Bytes result(size);
    for (std::size_t i = 0; i < size; ++i) result[i] = static_cast<std::uint8_t>(i * 73 + 5);
    return result;
}
template<class Predicate> static void until(sc::Platform& machine, Predicate predicate, unsigned limit = 100000) {
    while (!predicate() && limit--) {
        machine.step();
        if (!machine.fault().empty()) throw std::runtime_error(machine.fault());
    }
    require(predicate(), "platform test exceeded cycle limit");
}
static examples::guest::Image counter_image() {
    examples::guest::Assembler a;
    a.label("done"); a.addi(1, 1, 1); a.sw(1, 0, 0x60); a.beq(0, 0, "done");
    return a.finish().relocated(0x8000);
}
static examples::guest::Image timer_image() {
    using T = devices::Timer;
    examples::guest::Assembler a;
    a.li(1, sc::Platform::TimerBase); a.li(10, SoC::IrqBase);
    a.li_label(5, "handler"); a.csr(0, 5, 0x305);
    a.li(5, 1u << 11); a.csr(0, 5, 0x304);
    a.li(5, 8); a.csr(0, 5, 0x300);
    a.store_value(1, T::Compare, 100); a.store_value(1, T::Control, T::Enable | T::IrqEnable);
    a.label("done"); a.addi(9, 9, 1); a.beq(0, 0, "done");
    a.label("handler");
    a.lw(21, 10, soc::controller::Interrupt::Claim);
    a.li(22, 1); a.sw(22, 1, T::Pending); a.sw(21, 10, soc::controller::Interrupt::Complete);
    a.sw(21, 0, 0x60); a.emit(0x30200073u);
    return a.finish().relocated(0x8000);
}
static examples::guest::Image clint_image(bool timer, bool global) {
    using C = devices::Clint;
    examples::guest::Assembler a;
    a.li(1, sc::Platform::ClintBase); a.li(2, sc::Platform::ClintBase + C::MtimecmpLow);
    a.li_label(5, "vectors"); a.addi(5, 5, 1); a.csr(0, 5, 0x305);
    a.li(5, timer ? 128 : 8); a.csr(0, 5, 0x304);
    a.li(5, global ? 8 : 0); a.csr(0, 5, 0x300);
    if (timer) {
        a.store_value(2, 0, 0xffffffff); a.store_value(2, 4, 0); a.store_value(2, 0, 2048);
    }
    a.emit(0x10500073); // WFI. The next PC is the interrupt resume PC.
    a.store_value(0, 0x60, 77); a.label("done"); a.beq(0, 0, "done");
    a.label("vectors");
    for (unsigned n = 0; n < (timer ? 7u : 3u); ++n) a.emit(0x00000013);
    a.csr(10, 0, 0x342, true); a.csr(11, 0, 0x341, true);
    if (timer) { a.store_value(2, 4, 0xffffffff); a.store_value(2, 0, 0xffffffff); }
    else a.store_value(1, C::Msip, 0);
    a.sw(10, 0, 0x64); a.emit(0x30200073);
    return a.finish().relocated(0x8000);
}
static examples::guest::Image uart_image() {
    using U = devices::Uart;
    examples::guest::Assembler a;
    a.li(1, sc::Platform::UartBase);
    a.store_value(1, U::Control, U::RxIrq);
    a.store_value(1, U::Data, 'O'); a.store_value(1, U::Data, 'K');
    a.label("wait"); a.lw(6, 1, U::Status); a.li(7, U::TxReady);
    a.beq(6, 7, "wait"); a.li(7, U::TxReady | U::TxIdle); a.beq(6, 7, "wait");
    a.lw(6, 1, U::Data); a.sw(6, 1, U::Data);
    a.label("done"); a.beq(0, 0, "done");
    return a.finish().relocated(0x8000);
}
static void write_register(sc::Platform& machine, std::uint32_t address, std::uint32_t value) {
    const auto tag = machine.debug_port().submit(0, common::BusOperation::Write, address, 4, common::word_bytes(value));
    until(machine, [&] { return machine.debug_port().ready(tag); });
    require(machine.debug_port().take(tag).status == interconnect::pcie::Status::Success, "debug TLM MMIO write failed");
}
static common::Bytes command(std::uint8_t opcode, unsigned cid, std::uint64_t prp,
                             std::uint32_t nsid, std::uint32_t cdw10, std::uint32_t cdw11) {
    common::Bytes bytes(64);
    common::store32(bytes, 0, opcode | (cid << 16)); common::store32(bytes, 4, nsid);
    common::store32(bytes, 24, static_cast<std::uint32_t>(prp));
    common::store32(bytes, 40, cdw10); common::store32(bytes, 44, cdw11);
    return bytes;
}

static std::uint32_t narrow_load(unsigned funct3, unsigned rd, unsigned base, unsigned offset) {
    return (offset << 20) | (base << 15) | (funct3 << 12) | (rd << 7) | 3;
}
static std::uint32_t narrow_store(unsigned funct3, unsigned source, unsigned base, unsigned offset) {
    return ((offset >> 5) << 25) | (source << 20) | (base << 15) | (funct3 << 12) | ((offset & 31) << 7) | 0x23;
}

struct TrapFixture {
    examples::guest::Image image;
    std::uint32_t cause, pc, value;
};
static TrapFixture trap_image(const std::string& scenario) {
    std::uint32_t instruction = 0x00000073, cause = 11, value = 0, address = static_cast<std::uint32_t>(SoCConfig{}.memory.capacity_bytes());
    if (scenario == "trap_ebreak") { instruction = 0x00100073; cause = 3; }
    else if (scenario == "trap_illegal") { instruction = 0xffffffff; cause = 2; value = instruction; }
    else if (scenario == "trap_load") { instruction = narrow_load(2, 3, 1, 0); cause = 5; value = address; }
    else if (scenario == "trap_store") { instruction = narrow_store(2, 2, 1, 0); cause = 7; value = address; }
    else if (scenario == "trap_fetch") { instruction = 0x00008067; cause = 1; value = address; }
    else if (scenario == "trap_misaligned") { address = 0x7002; instruction = narrow_load(2, 3, 1, 0); cause = 4; value = address; }
    else if (scenario == "trap_csr") { instruction = 0xfff021f3; cause = 2; value = instruction; }
    else if (scenario == "trap_mmio") { address = sc::Platform::UartBase; instruction = narrow_load(4, 0, 1, 0); cause = 5; value = address; }
    examples::guest::Assembler a;
    a.li_label(5, "handler"); a.addi(5, 5, 1); a.csr(0, 5, 0x305); // Vectored
    a.li(1, address); a.li(2, 0x12345678);
    a.emit(instruction);
    a.label("resume"); a.store_value(0, 0x6c, 77); a.label("done"); a.beq(0, 0, "done");
    a.label("handler");
    a.csr(10, 0, 0x342, true); a.csr(11, 0, 0x341, true); a.csr(12, 0, 0x343, true);
    a.sw(10, 0, 0x60); a.sw(11, 0, 0x64); a.sw(12, 0, 0x68);
    if (scenario == "trap_fetch") a.li_label(11, "resume");
    else a.addi(11, 11, 4);
    a.csr(0, 11, 0x341); a.li(13, sc::Platform::UartBase); a.store_value(13, devices::Uart::Data, 'T');
    a.emit(0x30200073);
    auto image = a.finish().relocated(0x8000);
    const auto position = std::find(image.instructions.begin(), image.instructions.end(), instruction);
    require(position != image.instructions.end(), "trap fixture instruction missing");
    const auto pc = scenario == "trap_fetch" ? address : image.entry_point + 4 * std::uint32_t(position - image.instructions.begin());
    return {std::move(image), cause, pc, value};
}

int sc_main(int argc, char** argv) {
    try {
        const std::string scenario = argc > 1 ? argv[1] : "nvme";
        const bool machine_trap = scenario.compare(0, 5, "trap_") == 0;
        std::optional<TrapFixture> expected_trap;
        auto image = scenario == "nvme" ? examples::guest::nvme_read().relocated(0x8000)
            : scenario == "hdd" ? examples::guest::hdd_read(examples::guest::IoMode::DmaInterrupt).relocated(0x8000)
            : counter_image();
        if (scenario == "coherence") {
            examples::guest::Assembler a;
            a.li(1, 0x7000); a.label("done"); a.lw(2, 1, 0); a.sw(2, 0, 0x60); a.beq(0, 0, "done");
            image = a.finish().relocated(0x8000);
        }
        if (scenario == "timer") image = timer_image();
        if (scenario == "clint_timer" || scenario == "clint_software" || scenario == "clint_reset" || scenario == "wfi_masked_global")
            image = clint_image(scenario == "clint_timer" || scenario == "clint_reset", scenario != "wfi_masked_global");
        if (machine_trap) { expected_trap = trap_image(scenario); image = expected_trap->image; }
        if (scenario == "uart") image = uart_image();
        if (scenario == "boot_jump") {
            image.instructions = {0x123450b7, 0xfffff117, 0x00c002ef, 0x05800193, 0x06300193,
                                  0x01128293, 0x000282e7, 0x00028313, 0x06602023, 0x00000063};
            image.constants.clear();
        }
        if (scenario == "boot_branch") {
            image.instructions = {0xfff00093, 0x00100113, 0x0020c463, 0x06202023,
                                  0x04d00193, 0x06302223, 0x00000063};
            image.constants.clear();
        }
        if (scenario == "integer") {
            image.instructions = {0xfff00093, 0x0010d113, 0x001111b3, 0x41f1d213, 0xfff24313,
                                  0x0ff17393, 0x0033e433, 0x06802023, 0x00000063};
            image.constants.clear();
        }
        if (scenario == "fence_i") {
            examples::guest::Assembler a;
            a.li(1, 0x8100); a.emit(0x000082e7); // JALR x5,x1,0: warm the old function in I-cache.
            a.li(2, 0x00200313); a.sw(2, 1, 0); a.emit(0x0000100f);
            a.emit(0x000082e7); a.sw(6, 0, 0x60); a.label("done"); a.beq(0, 0, "done");
            image = a.finish().relocated(0x8000);
            image.instructions.resize(64, 0x00000013);
            image.instructions.push_back(0x00100313); // ADDI x6,x0,1 -> patched to 2.
            image.instructions.push_back(0x00028067); // JALR x0,x5,0.
        }
        if (scenario == "fence_dma") {
            examples::guest::Assembler a;
            a.li(1, SoC::NicBase); a.li(2, 0x7000); a.li(3, 0x7100);
            a.store_value(1, devices::NIC::TxAddress, 0x7000); a.store_value(1, devices::NIC::RxAddress, 0x7100);
            a.store_value(1, devices::NIC::Length, 4);
            a.li(4, 0x12345678); a.sw(4, 2, 0); a.emit(0x0ff0000f);
            a.store_value(1, devices::NIC::Command, 1);
            a.label("done"); a.label("wait"); a.lw(6, 3, 0); a.sw(6, 0, 0x60); a.beq(0, 0, "wait");
            image = a.finish().relocated(0x8000);
        }
        if (scenario == "subword") {
            examples::guest::Assembler a;
            a.li(1, 0x7000); a.li(12, 0x12345678);
            a.emit(narrow_load(0, 3, 1, 0)); a.emit(narrow_load(4, 4, 1, 0));
            a.emit(narrow_load(1, 5, 1, 2)); a.emit(narrow_load(5, 6, 1, 2));
            a.emit(narrow_store(0, 12, 1, 1)); a.emit(narrow_store(1, 12, 1, 2));
            a.lw(7, 1, 0);
            // Cold store is write-through without cache allocation.
            a.emit(narrow_store(1, 12, 1, 66)); a.emit(narrow_load(5, 8, 1, 66));
            a.sw(7, 0, 0x60); a.label("done"); a.beq(0, 0, "done");
            image = a.finish().relocated(0x8000);
            image.constants.push_back({0x7000, 0x80807fff});
            image.constants.push_back({0x7040, 0xaabbccdd});
        }
        if (scenario == "subword_mmio_load" || scenario == "subword_mmio_store") {
            examples::guest::Assembler a;
            a.li(1, sc::Platform::UartBase); a.li(2, 'X');
            a.emit(scenario == "subword_mmio_load" ? narrow_load(4, 0, 1, 0) : narrow_store(1, 2, 1, 0));
            a.label("done"); a.beq(0, 0, "done");
            image = a.finish().relocated(0x8000);
        }
        if (scenario == "illegal") image.instructions[0] = 0xFFFFFFFF;
        if (scenario == "fetch_fault") {
            image.entry_point = static_cast<std::uint32_t>(SoCConfig{}.memory.capacity_bytes() - 4); image.instructions = {0x00100093}; image.constants.clear();
        }
        SoCConfig config; config.disk.spin_up_ms = 1;
        sc::BootImage boot{image.entry_point, image.instructions, image.constants};
        if (scenario == "elf") {
            auto elf = simulation::boot::parse_elf32(elf_fixture(), config.memory.capacity_bytes());
            boot = {}; boot.entry = elf.entry; boot.segments = std::move(elf.segments);
        }
        sc::Platform machine("computer", std::move(boot),
            sc_core::sc_time(50, sc_core::SC_US), config,
            scenario == "illegal" || scenario == "fetch_fault" || scenario == "subword_mmio_load" || scenario == "subword_mmio_store"
                ? soc::core::FaultHandling::HostException : soc::core::FaultHandling::MachineTrap);
        machine.nand().write(0, pattern());
        if (scenario == "hdd" || scenario == "multi_master") {
            machine.disk().power_on(); machine.disk().advance(1);
            machine.disk().submit_write_lba(0, pattern());
            require(machine.disk().run_until_idle(100), "HDD fixture timeout");
            const auto result = machine.disk().take_completion();
            require(result && result->status == devices::hdd::Status::Success, "HDD fixture failed");
        }
        require(!machine.powered() && machine.cpu().completed_instructions() == 0, "construction executed CPU");
        machine.power_on();
        if (scenario == "trap_mmio") machine.uart().inject("Q");
        if (scenario == "subword_mmio_load" || scenario == "subword_mmio_store") machine.uart().inject("Q");
        require(machine.cpu().pc() == image.entry_point && !machine.cpu().clock_pulses(), "CPU ran during reset");
        machine.step();
        require(machine.cycles.read() == 1 && machine.cpu().clock_pulses() == 1, "single step was not exactly one clock");

        if (machine_trap) {
            require(machine.cpu().fault_handling() == soc::core::FaultHandling::MachineTrap, "platform did not select machine traps");
            until(machine, [&] { return common::load32(machine.memory().read(0x6c, 4)) == 77; });
            require(machine.fault().empty() && machine.cpu().interrupts().exceptions() == 1 &&
                machine.cpu().interrupts().entries() == 0 && machine.cpu().interrupts().returns() == 1,
                "SystemC guest trap did not enter and return exactly once");
            require(common::load32(machine.memory().read(0x60, 4)) == expected_trap->cause &&
                common::load32(machine.memory().read(0x64, 4)) == expected_trap->pc &&
                common::load32(machine.memory().read(0x68, 4)) == expected_trap->value,
                "guest handler saved incorrect exception CSRs");
            require(machine.exception_entries.read() == 1 && machine.mcause.read() == expected_trap->cause &&
                machine.mtval.read() == expected_trap->value, "trap monitoring signals differed from hardware");
            until(machine, [&] { return machine.uart().output_size() == 1; });
            require(machine.uart().take_output() == "T", "guest trap handler failed UART output");
            if (scenario == "trap_mmio") require(machine.uart().mmio_read32(devices::Uart::Data) == 'Q',
                "rejected narrow load consumed UART input before trap");
            machine.reset();
            require(machine.cpu().interrupts().exceptions() == 0 && machine.cpu().interrupts().read(0x343) == 0 &&
                machine.cpu().fault_handling() == soc::core::FaultHandling::MachineTrap,
                "reset retained trap state or lost machine policy");
        } else if (scenario == "subword_mmio_load" || scenario == "subword_mmio_store") {
            for (unsigned i = 0; i < 1000 && machine.fault().empty(); ++i) machine.step();
            require(machine.fault().find(scenario == "subword_mmio_load" ? "cause=5" : "cause=7") != std::string::npos,
                "narrow MMIO access did not fault");
            require(machine.uart().mmio_read32(devices::Uart::Status) & devices::Uart::RxReady,
                "rejected MMIO load consumed RX data");
            require(machine.uart().mmio_read32(devices::Uart::Data) == 'Q' && machine.uart().output_size() == 0 &&
                (machine.uart().mmio_read32(devices::Uart::Status) & devices::Uart::TxIdle),
                "rejected narrow MMIO access had a device side effect");
        } else if (scenario == "illegal" || scenario == "fetch_fault") {
            for (unsigned i = 0; i < 200 && machine.fault().empty(); ++i) machine.step();
            require(!machine.fault().empty(), "instruction path failed to report CPU fault");
            require(machine.fault().find(scenario == "illegal" ? "cause=2" : "cause=1") != std::string::npos,
                    "wrong instruction fault cause");
            require(machine.cpu().completed_instructions() == (scenario == "illegal" ? 0u : 1u), "fault retired an instruction");
        } else if (scenario == "nvme" || scenario == "hdd") {
            until(machine, [&] { return common::load32(machine.memory().read(0x10, 4)) == 1; });
            const auto destination = scenario == "nvme" ? 0x5000 : 0x800;
            require(machine.memory().read(destination, 512) == pattern(), "guest DMA data mismatch");
            require(machine.cpu().registers().read(28) == common::load32(pattern()), "guest CPU did not read DMA data");
            require(machine.instruction_cache().cache().statistics().read_hits > 0 &&
                    machine.instruction_cache().cache().statistics().fills > 0, "instruction fetch bypassed cache");
            require(machine.data_cache().cache().statistics().fills > 0, "guest data bypassed cache");
            require(machine.cpu().bus_waits() > 0, "CPU did not wait for asynchronous memory");
            if (scenario == "nvme") require(machine.nvme().completed_commands() == 3, "NVMe queue commands missing");
            else require(machine.hdd().dma().transferred() == 512 && machine.cpu().interrupts().entries() > 0 &&
                         machine.cpu().interrupts().returns() > 0 && machine.cpu().registers().read(9) > 0,
                         "DMA/independent CPU work/ISR return missing");
        } else if (scenario == "clint_reset") {
            until(machine, [&] { return machine.cpu().waiting(); });
            write_register(machine, sc::Platform::ClintBase + devices::Clint::Msip, 1);
            require(machine.cpu().waiting() && machine.msip.read() && (machine.cpu().interrupts().read(0x344) & 8),
                "reset fixture did not hold a masked software IRQ while CPU slept");
            machine.reset();
            require(!machine.cpu().waiting() && !machine.cpu().wfi_wait_cycles() && machine.clint().time() == 0 &&
                machine.clint().compare() == UINT64_MAX && !machine.msip.read() && !machine.mtip.read(),
                "reset did not clear sleeping CPU and published CLINT levels");
            machine.step();
            require(machine.cpu().clock_pulses() == 1 && !machine.cpu().interrupts().entries() &&
                machine.cpu().interrupts().read(0x344) == 0, "stale local IRQ reappeared after reset");
        } else if (scenario == "clint_timer" || scenario == "clint_software" || scenario == "wfi_masked_global") {
            until(machine, [&] { return machine.cpu().waiting(); });
            const auto resume = machine.cpu().pc();
            const auto retired = machine.cpu().completed_instructions();
            const auto hits = machine.instruction_cache().cache().statistics().read_hits;
            const auto time = machine.clint().time();
            machine.run(20);
            require(machine.cpu().waiting() && machine.cpu().pc() == resume && machine.cpu().completed_instructions() == retired &&
                machine.instruction_cache().cache().statistics().read_hits == hits && machine.clint().time() == time + 20 &&
                machine.cpu().wfi_wait_cycles() >= 20, "WFI failed to stop instruction fetch while keeping timer running");
            const auto paused = machine.clint().time();
            machine.pause(); sc_core::sc_start(500, sc_core::SC_US);
            require(machine.clint().time() == paused && machine.cpu().waiting(), "debugger pause advanced CLINT");
            const bool timer = scenario == "clint_timer", global = scenario != "wfi_masked_global";
            if (timer) {
                // Configure an independent device through the debug TLM master while CPU sleeps.
                machine.memory().write(0x7000, pattern(128));
                write_register(machine, SoC::NicBase + devices::NIC::TxAddress, 0x7000);
                write_register(machine, SoC::NicBase + devices::NIC::RxAddress, 0x7100);
                write_register(machine, SoC::NicBase + devices::NIC::Length, 128);
                write_register(machine, SoC::NicBase + devices::NIC::Command, 1);
                until(machine, [&] { return machine.nic().mmio_read32(devices::NIC::Status) == 2; });
                require(machine.memory().read(0x7100, 128) == pattern(128) && machine.cpu().waiting() &&
                    machine.cpu().completed_instructions() == retired, "DMA failed to progress independently of sleeping CPU");
                until(machine, [&] { return machine.cpu().interrupts().entries() == 1; });
                require(machine.cpu().pc() == (machine.cpu().interrupts().read(0x305) & ~3u) + 28,
                    "machine timer used wrong vectored slot");
            } else write_register(machine, sc::Platform::ClintBase + devices::Clint::Msip, 1);
            until(machine, [&] { return common::load32(machine.memory().read(0x60, 4)) == 77; });
            require(!machine.cpu().waiting() && machine.cpu().clock_pulses() == machine.cycles.read() &&
                machine.clint().time() == machine.cycles.read() && machine.timebase_hz() == 20000,
                "WFI/local timer clock accounting differed");
            if (global) {
                const auto cause = timer ? 7u : 3u;
                require(machine.cpu().interrupts().entries() == 1 && machine.cpu().interrupts().returns() == 1 &&
                    machine.cpu().interrupts().read(0x341) == resume &&
                    common::load32(machine.memory().read(0x64, 4)) == (0x80000000u | cause),
                    "CLINT IRQ cause, saved PC or return differed");
                machine.run(200);
                require(machine.cpu().interrupts().entries() == 1 &&
                    !(machine.cpu().interrupts().read(0x344) & (timer ? 128u : 8u)), "acknowledged CLINT IRQ repeated");
            } else {
                require(!machine.cpu().interrupts().entries() && machine.cpu().interrupts().read(0x344) == 8,
                    "global MIE prevented WFI wake or allowed a trap");
                write_register(machine, sc::Platform::ClintBase + devices::Clint::Msip, 0);
            }
            machine.reset();
            require(machine.clint().time() == 0 && machine.clint().compare() == UINT64_MAX &&
                !machine.clint().software_pending() && !machine.cpu().waiting() && !machine.mtip.read() && !machine.msip.read(),
                "reset retained CLINT/WFI state");
        } else if (scenario == "fence_i") {
            until(machine, [&] { return machine.cpu().registers().read(6) == 1; });
            require(machine.instruction_cache().cache().contains(0x8100, 4) &&
                common::load32(machine.memory().read(0x8100, 4)) == 0x00100313,
                "self-modifying fixture did not warm original instruction cache line");
            until(machine, [&] { return common::load32(machine.memory().read(0x60, 4)) == 2; });
            require(common::load32(machine.memory().read(0x8100, 4)) == 0x00200313 &&
                !machine.cpu().interrupts().exceptions(), "FENCE.I fetched old code or raised a trap");
        } else if (scenario == "fence_dma") {
            until(machine, [&] { return common::load32(machine.memory().read(0x60, 4)) == 0x12345678; });
            require(machine.nic().mmio_read32(devices::NIC::Status) == 2 &&
                common::load32(machine.memory().read(0x7100, 4)) == 0x12345678 &&
                machine.cpu().bus_waits() && !machine.cpu().interrupts().exceptions(),
                "fenced data/doorbell/DMA ordering differed");
        } else if (scenario == "subword") {
            until(machine, [&] { return common::load32(machine.memory().read(0x60, 4)) == 0x567878ff; });
            require(machine.cpu().registers().read(3) == 0xffffffff && machine.cpu().registers().read(4) == 255 &&
                machine.cpu().registers().read(5) == 0xffff8080 && machine.cpu().registers().read(6) == 0x8080 &&
                machine.cpu().registers().read(8) == 0x5678, "SystemC subword result differed");
            require(common::load32(machine.memory().read(0x7040, 4)) == 0x5678ccdd,
                "cold halfword write changed neighboring bytes");
            require(machine.data_cache().cache().statistics().read_hits >= 4 &&
                machine.data_cache().cache().statistics().write_hits >= 2 &&
                machine.data_cache().cache().statistics().write_misses >= 1,
                "subword access bypassed cache or failed to exercise write policies");
            const auto tag = machine.debug_port().submit(0, common::BusOperation::Write, 0x7002, 2, {0x34, 0x12});
            until(machine, [&] {
                const auto visible = common::load_scalar(machine.memory().read(0x7002, 2));
                require(visible == 0x5678 || visible == 0x1234, "aligned halfword store exposed a torn value");
                return machine.debug_port().ready(tag);
            });
            require(machine.debug_port().take(tag).status == interconnect::pcie::Status::Success,
                "debug halfword store failed");
            const auto& cache = machine.data_cache().cache();
            const auto parts = cache.address_decoder().decode(0x7000);
            bool updated = false;
            for (std::size_t way = 0; way < cache.config().ways; ++way) {
                const auto& line = cache.line(parts.set_index, way);
                if (line.valid && line.tag == parts.tag) {
                    updated = true;
                    require(common::load32(line.data) == 0x123478ff, "partial external write left a stale cache lane");
                }
            }
            require(updated, "external halfword write discarded the cached line");
        } else if (scenario == "integer") {
            until(machine, [&] { return common::load32(machine.memory().read(0x60, 4)) == 0x800000ff; });
            require(machine.cpu().registers().read(4) == 0xffffffff && machine.cpu().registers().read(6) == 0,
                    "integer program produced wrong shift/mask result");
        } else if (scenario == "boot_branch") {
            until(machine, [&] { return common::load32(machine.memory().read(0x64, 4)) == 77; });
            require(common::load32(machine.memory().read(0x60, 4)) == 0 && machine.cpu().registers().read(3) == 77,
                    "signed branch or wrong-path store in boot program failed");
        } else if (scenario == "boot_jump") {
            until(machine, [&] { return common::load32(machine.memory().read(0x60, 4)) == 0x801c; });
            require(machine.cpu().registers().read(1) == 0x12345000 && machine.cpu().registers().read(2) == 0x7004 &&
                    machine.cpu().registers().read(3) == 0 && machine.cpu().registers().read(6) == 0x801c,
                    "boot address calculation or call/return failed");
        } else if (scenario == "elf") {
            require(machine.memory().read(0x204, 12) == common::Bytes(12, 0), "ELF BSS was not zeroed");
            until(machine, [&] { return common::load32(machine.memory().read(0x60, 4)) == 0xcafebabe; });
            require(machine.cpu().completed_instructions() >= 2, "ELF code did not execute");
            machine.memory().write(0x204, common::word_bytes(7));
            machine.reset();
            require(common::load32(machine.memory().read(0x204, 4)) == 7, "warm reset cleared retained RAM");
            machine.power_off(); machine.power_on();
            require(common::load32(machine.memory().read(0x204, 4)) == 0, "cold boot did not restore BSS");
        } else if (scenario == "uart") {
            using U = devices::Uart;
            until(machine, [&] { return machine.uart().output_size() == 2; });
            require(machine.uart().take_output() == "OK", "guest UART output differed");
            machine.uart().inject("!");
            until(machine, [&] { return machine.irq_pending.read(); });
            until(machine, [&] { return machine.uart().output_size() == 1; });
            require(machine.uart().take_output() == "!" && !machine.irq_pending.read(), "UART input/echo/IRQ failed");
            require(!(machine.uart().mmio_read32(U::Status) & U::RxReady), "guest did not consume RX byte");
            machine.uart().inject(std::string(U::FifoCapacity, 'x'));
            bool overflow = false;
            try { machine.uart().inject("y"); } catch (const std::overflow_error&) { overflow = true; }
            require(overflow, "UART accepted input beyond FIFO capacity");
            machine.reset();
            require(!(machine.uart().mmio_read32(U::Status) & U::RxReady) &&
                    machine.uart().mmio_read32(U::Control) == 0, "UART reset retained volatile state");
        } else if (scenario == "timer") {
            until(machine, [&] { return machine.cpu().interrupts().returns() == 1; });
            require(common::load32(machine.memory().read(0x60, 4)) == 4, "guest claimed wrong timer IRQ source");
            require(machine.cpu().interrupts().entries() == 1 && machine.cpu().registers().read(9) > 0,
                    "CPU did not work independently before timer interrupt");
            machine.run(500);
            require(machine.cpu().interrupts().entries() == 1 && !machine.timer().mmio_read32(devices::Timer::Pending),
                    "acknowledged one-shot timer fired again");
            machine.reset();
            require(machine.timer().mmio_read32(devices::Timer::Control) == 0 &&
                    machine.timer().mmio_read32(devices::Timer::Count) == 0, "timer reset failed");
        } else if (scenario == "timer_periodic") {
            using T = devices::Timer;
            const auto invalid = machine.debug_port().submit(0, common::BusOperation::Write,
                sc::Platform::TimerBase + T::Control, 4, common::word_bytes(T::Enable));
            until(machine, [&] { return machine.debug_port().ready(invalid); });
            require(machine.debug_port().take(invalid).status != interconnect::pcie::Status::Success &&
                    machine.timer().mmio_read32(T::Control) == 0, "zero timer threshold was accepted");
            write_register(machine, sc::Platform::TimerBase + T::Compare, 10);
            write_register(machine, sc::Platform::TimerBase + T::Control, T::Enable | T::Periodic);
            machine.run(30);
            require(machine.timer().mmio_read32(T::Pending) == 1 &&
                    (machine.timer().mmio_read32(T::Control) & T::Enable) &&
                    machine.timer().mmio_read32(T::Count) < 10, "periodic timer failed to reload and latch IRQ");
            require(!machine.irq_pending.read(), "masked timer raised IRQ");
            write_register(machine, sc::Platform::TimerBase + T::Control, T::Enable | T::Periodic | T::IrqEnable);
            require(machine.irq_pending.read(), "unmasking a pending timer failed to raise IRQ");
            write_register(machine, sc::Platform::TimerBase + T::Control, T::IrqEnable);
            write_register(machine, sc::Platform::TimerBase + T::Pending, 1);
            machine.run(30);
            require(!machine.timer().mmio_read32(T::Pending) && !machine.irq_pending.read(),
                    "disabled timer reasserted after acknowledgement");
        } else if (scenario == "coherence") {
            until(machine, [&] { return machine.data_cache().cache().contains(0x7000, 4); });
            const auto fills = machine.data_cache().cache().statistics().fills;
            machine.memory().write(0x7100, common::word_bytes(0xCAFEBABE));
            write_register(machine, SoC::NicBase + devices::NIC::TxAddress, 0x7100);
            write_register(machine, SoC::NicBase + devices::NIC::RxAddress, 0x7000);
            write_register(machine, SoC::NicBase + devices::NIC::Length, 4);
            write_register(machine, SoC::NicBase + devices::NIC::Command, 1);
            until(machine, [&] { return common::load32(machine.memory().read(0x60, 4)) == 0xCAFEBABE; });
            require(machine.data_cache().cache().statistics().fills == fills &&
                    machine.data_cache().cache().statistics().read_hits > 0,
                    "CPU did not observe DMA through its already-cached copy");
        } else if (scenario == "multi_master") {
            using C = devices::nvme::Controller;
            using H = sc::Platform::HddController;
            write_register(machine, SoC::NvmeBase + C::AdminAttributes, 0x30003);
            write_register(machine, SoC::NvmeBase + C::AdminSq, 0x1000);
            write_register(machine, SoC::NvmeBase + C::AdminCq, 0x2000);
            write_register(machine, SoC::NvmeBase + C::Config, 0x460001);
            until(machine, [&] { return machine.nvme().state() == C::State::Idle; });
            machine.memory().write(0x1000, command(5, 1, 0x4000, 0, 0x30001, 3));
            machine.memory().write(0x1040, command(1, 2, 0x3000, 0, 0x30001, 0x10001));
            write_register(machine, SoC::NvmeBase + C::Doorbell, 2);
            until(machine, [&] { return machine.nvme().completed_commands() == 2; });
            write_register(machine, SoC::NvmeBase + C::Doorbell + 4, 2);
            machine.memory().write(0x3000, command(2, 10, 0x5000, 1, 0, 0));
            machine.memory().write(0x7000, pattern(128));
            write_register(machine, SoC::NicBase + devices::NIC::TxAddress, 0x7000);
            write_register(machine, SoC::NicBase + devices::NIC::RxAddress, 0x7800);
            write_register(machine, SoC::NicBase + devices::NIC::Length, 128);
            write_register(machine, SoC::HddBase + H::Length, 512);
            write_register(machine, SoC::HddBase + H::DmaAddress, 0x6000);
            write_register(machine, SoC::HddBase + H::Control, H::EnableDma);
            write_register(machine, SoC::HddBase + H::Command, H::Read);
            write_register(machine, SoC::NvmeBase + C::Doorbell + 8, 1);
            write_register(machine, SoC::NicBase + devices::NIC::Command, 1);
            std::set<unsigned> masters;
            until(machine, [&] {
                if (machine.bus().fabric().last_tick().master) masters.insert(*machine.bus().fabric().last_tick().master);
                return machine.hdd().state() == devices::hdd::DeviceState::Completed &&
                    machine.nvme().completed_commands() == 3 &&
                    machine.memory().read(0x7800, 128) == pattern(128);
            });
            require(machine.memory().read(0x5000, 512) == pattern() && machine.memory().read(0x6000, 512) == pattern(),
                    "multi-master DMA data differs");
            require(masters.count(0) && masters.count(1) && masters.count(2) && masters.count(3),
                    "not every CPU/DMA master received bus service");
            require(common::load32(machine.memory().read(0x60, 4)) > 0, "CPU failed to progress during DMA");
        } else {
            const auto before = sc_core::sc_time_stamp();
            machine.run(4);
            require(sc_core::sc_time_stamp() - before == sc_core::sc_time(200, sc_core::SC_US),
                    "successive run/step calls skipped valid clock edges");
            until(machine, [&] { return common::load32(machine.memory().read(0x60, 4)) >= 3; });
            const auto cycles = machine.cycles.read(), pulses = machine.cpu().clock_pulses();
            const auto pc = machine.cpu().pc();
            machine.pause();
            sc_core::sc_start(1, sc_core::SC_MS);
            require(machine.cycles.read() == cycles && machine.cpu().clock_pulses() == pulses && machine.cpu().pc() == pc,
                    "pause did not stop the hardware domain");
            machine.memory().write(0x7000, common::word_bytes(0x12345678));
            until(machine, [&] { return !machine.bus().idle(); });
            machine.reset(); // Cancel a live instruction/data transaction, preserving storage.
            require(machine.bus().idle() && machine.cpu().pc() == 0x8000 && !machine.cpu().completed_instructions(),
                    "reset did not clear in-flight bus/CPU state");
            require(common::load32(machine.memory().read(0x7000, 4)) == 0x12345678 && machine.nand().read(0, 1) == pattern(),
                    "reset lost RAM or NAND");
            until(machine, [&] { return machine.cpu().completed_instructions() >= 4; });
            machine.iommu().map({SoC::NvmeMaster, 0x7000, 0x7000, 4, true, false});
            machine.iommu().enable();
            const auto tag = machine.debug_port().submit(SoC::NvmeMaster, common::BusOperation::Write,
                                                         0x7000, 4, common::word_bytes(0));
            until(machine, [&] { return machine.debug_port().ready(tag); });
            require(machine.debug_port().take(tag).status != interconnect::pcie::Status::Success &&
                    common::load32(machine.memory().read(0x7000, 4)) == 0x12345678, "IOMMU denied write changed RAM");
            machine.power_off();
            sc_core::sc_start(1, sc_core::SC_MS);
            require(!machine.cpu().clock_pulses(), "CPU ran while powered off");
            machine.power_on();
            require(common::load32(machine.memory().read(0x7000, 4)) == 0 && machine.nand().read(0, 1) == pattern(),
                    "cold power cycle did not clear RAM/preserve NAND");
            until(machine, [&] { return machine.cpu().completed_instructions() >= 4; });
        }
        std::cout << "PASS platform " << scenario << ": cycles=" << machine.cycles.read()
                  << " retired=" << machine.cpu().completed_instructions()
                  << " instruction-fills=" << machine.instruction_cache().cache().statistics().fills
                  << " IRQs=" << machine.cpu().interrupts().entries() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL platform: " << error.what() << '\n';
        return 1;
    }
}
