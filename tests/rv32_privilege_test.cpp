#include "../soc/rv32/core.hpp"
#include <iostream>

using CPU = colab::soc::RV32Core;
using Privilege = colab::soc::core::PrivilegeMode;
using Cause = colab::soc::core::RV32FaultCause;
using State = colab::soc::core::InterruptSystem;
namespace common = colab::devices::common;
static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static std::uint32_t csr(unsigned number, unsigned rd, unsigned source = 0, bool read = true) {
    return (number << 20) | (source << 15) | (read ? 0x2000u : 0x1000u) | (rd << 7) | 0x73;
}
static std::vector<std::uint32_t> image(std::uint32_t instruction) {
    std::vector<std::uint32_t> program(64, 0x00000063);
    program[0] = 0x30200073; // Bootstrap MRET to user PC=64.
    program[16] = instruction; program[17] = 0x00700213; // ADDI x4,x0,7 after the fault.
    program[32] = csr(0x342, 10); program[33] = csr(0x341, 11); program[34] = csr(0x343, 12);
    program[35] = csr(0x300, 13); program[36] = 0x00458593; // Advance mepc by 4.
    program[37] = csr(0x341, 0, 11, false); program[38] = 0x30200073;
    return program;
}
static CPU make_cpu(std::vector<std::uint32_t> program, unsigned config) {
    return CPU(std::move(program), config == 1 ? CPU::Mode::Microprogrammed : CPU::Mode::Hardwired, 256, {},
        config >= 2 ? CPU::Execution::Pipelined : CPU::Execution::MultiCycle, {config != 3},
        colab::soc::core::FaultHandling::MachineTrap);
}
static void prepare(CPU& cpu) {
    auto& state = cpu.interrupts();
    state.write(0x3b0, 64 / 4); state.write(0x3b1, 128 / 4); // TOR: user RX code [64,128).
    state.write(0x3b2, (0x200 / 4) | 7); // NAPOT: user RW data [0x200,0x240).
    state.write(0x3a0, 0x001b0d00);
    state.write(0x305, 128); state.write(0x341, 64); state.write(0x300, 0);
}
class Bus : public common::DmaBus {
public:
    common::Bytes memory = common::Bytes(1024);
    unsigned reads = 0, writes = 0, submits = 0;
    common::Tag submit(common::Master, common::BusOperation operation, std::uint64_t address,
                       std::size_t count, common::Bytes data = {}) override {
        require(!pending && !response, "overlapping user request");
        ++submits; op = operation; at = address; bytes = count; payload = std::move(data); pending = true; delay = 3;
        return submits;
    }
    bool ready(common::Tag tag) const override { return response && response->tag == tag; }
    common::BusResult take(common::Tag tag) override {
        require(ready(tag), "user response not ready"); auto result = *response; response.reset(); return result;
    }
    void tick() {
        if (!pending || --delay) return;
        common::BusResult result{}; result.tag = submits;
        result.status = colab::interconnect::pcie::Status::Unmapped;
        if (at <= memory.size() && bytes <= memory.size() - at) {
            result.status = colab::interconnect::pcie::Status::Success; result.transferred_bytes = bytes;
            if (op == common::BusOperation::Read) { ++reads; result.data.assign(memory.begin() + at, memory.begin() + at + bytes); }
            else { ++writes; std::copy(payload.begin(), payload.end(), memory.begin() + at); }
        }
        pending = false; response = result;
    }
private:
    bool pending = false;
    unsigned delay = 0;
    common::BusOperation op{};
    std::uint64_t at = 0;
    std::size_t bytes = 0;
    common::Bytes payload;
    std::optional<common::BusResult> response;
};
template<class Predicate> static void until(CPU& cpu, Bus* bus, Predicate condition) {
    for (unsigned n = 0; n < 1000 && !condition(); ++n) { if (bus) bus->tick(); cpu.tick(); }
    require(condition(), "privilege test timed out");
}
static void trap_cases() {
    struct Case { std::uint32_t instruction, address; Cause cause; std::uint32_t value; };
    const std::vector<Case> cases{
        {0x00000073, 0, Cause::EnvironmentCallFromU, 0},
        {0x30200073, 0, Cause::IllegalInstruction, 0x30200073},
        {csr(0x305, 3), 0, Cause::IllegalInstruction, csr(0x305, 3)},
        {csr(0x3a0, 0), 0, Cause::IllegalInstruction, csr(0x3a0, 0)},
        {csr(0x305, 0, 0, false), 0, Cause::IllegalInstruction, csr(0x305, 0, 0, false)},
        {0x0000a183, 0x300, Cause::LoadAccessFault, 0x300},
        {0x0020a023, 0x300, Cause::StoreAccessFault, 0x300},
        {0x0000a003, 0x40001000, Cause::LoadAccessFault, 0x40001000},
        {0x0020a023, 0x40001000, Cause::StoreAccessFault, 0x40001000}
    };
    for (unsigned config = 0; config < 4; ++config) for (bool external : {false, true}) for (const auto& item : cases) {
        auto cpu = make_cpu(image(item.instruction), config); prepare(cpu); Bus bus;
        cpu.set_register(1, item.address); cpu.set_register(2, 0x12345678); cpu.set_register(3, 99);
        if (external) cpu.connect_bus(bus);
        until(cpu, external ? &bus : nullptr, [&] { return cpu.interrupts().exceptions() == 1; });
        require(cpu.pc() == 128 && cpu.interrupts().privilege() == Privilege::Machine &&
            cpu.interrupts().read(0x341) == 64 && cpu.interrupts().read(0x342) == unsigned(item.cause) &&
            cpu.interrupts().read(0x343) == item.value && !(cpu.interrupts().read(0x300) & State::MPP),
            "U-mode exception saved wrong privilege/PC/cause/value");
        require(cpu.completed_instructions() == 1 && cpu.registers().read(3) == 99 && !bus.submits &&
            cpu.interrupts().read(0x305) == 128, "forbidden user operation escaped or retired");
        until(cpu, external ? &bus : nullptr, [&] { return cpu.registers().read(4) == 7; });
        require(cpu.interrupts().privilege() == Privilege::User && cpu.interrupts().returns() == 2 &&
            cpu.interrupts().exceptions() == 1, "handler did not return to U");
    }
}
static void memory_and_interrupts() {
    for (unsigned config = 0; config < 4; ++config) for (bool external : {false, true}) {
        auto program = image(0x0000a183); // LW x3,0(x1), ADDI, SW, LW.
        program[17] = 0x00118213; program[18] = 0x0040a223; program[19] = 0x0040a283;
        auto cpu = make_cpu(program, config); prepare(cpu); Bus bus;
        cpu.set_register(1, 0x200); cpu.set_memory(0x200, 41); bus.memory[0x200] = 41;
        if (external) cpu.connect_bus(bus);
        until(cpu, external ? &bus : nullptr, [&] { return cpu.completed_instructions() == 5; });
        require(cpu.registers().read(3) == 41 && cpu.registers().read(5) == 42 &&
            cpu.interrupts().privilege() == Privilege::User && !cpu.interrupts().exceptions(), "permitted U memory flow failed");
        if (external) require(bus.reads == 2 && bus.writes == 1 && cpu.bus_waits(), "U memory bypassed delayed bus");

        auto jump = make_cpu(image(0x00008067), config); prepare(jump); jump.set_register(1, 0x200);
        until(jump, nullptr, [&] { return jump.interrupts().exceptions() == 1; });
        require(jump.interrupts().read(0x342) == 1 && jump.interrupts().read(0x341) == 0x200 &&
            jump.interrupts().read(0x343) == 0x200 && jump.completed_instructions() == 2,
            "user executed a data-only region");

        auto irq_program = image(0x00000063); irq_program[32] = 0x30200073;
        auto irq = make_cpu(irq_program, config); prepare(irq); irq.interrupts().write(0x304, State::MEIE);
        until(irq, nullptr, [&] { return irq.interrupts().privilege() == Privilege::User; });
        require(!(irq.interrupts().read(0x300) & State::MIE), "IRQ test requires MIE=0 in U");
        irq.interrupts().set_external(true); irq.tick();
        require(irq.interrupts().privilege() == Privilege::Machine && irq.interrupts().entries() == 1 &&
            !(irq.interrupts().read(0x300) & State::MPP), "higher-privilege IRQ was masked by U MIE");
        irq.interrupts().set_external(false);
        until(irq, nullptr, [&] { return irq.interrupts().privilege() == Privilege::User; });
        require(irq.pc() == 64 && irq.interrupts().returns() == 2, "IRQ MRET did not restore U PC");

        auto mprv = make_cpu({0x0000a183}, config); prepare(mprv); mprv.set_register(1, 0x300);
        mprv.interrupts().write(0x300, State::MPRV);
        until(mprv, nullptr, [&] { return mprv.interrupts().exceptions() == 1; });
        require(mprv.interrupts().read(0x342) == 5 && mprv.interrupts().read(0x341) == 0 &&
            mprv.interrupts().privilege() == Privilege::Machine, "MPRV did not constrain M data access");
    }
    for (unsigned config = 0; config < 4; ++config) for (bool user_page : {false, true}) {
        auto cpu = make_cpu(image(0x0000a183), config); prepare(cpu); Bus bus;
        cpu.set_register(1, 0x2000); cpu.connect_bus(bus);
        cpu.mmu().enable_paging(0, [user_page](auto) -> colab::soc::core::mmu::PageTableEntry {
            return {0, true, {true, true, true, user_page}, true, false, 0};
        });
        until(cpu, &bus, [&] { return cpu.interrupts().exceptions() == 1; });
        require(cpu.interrupts().read(0x342) == (user_page ? 5u : 13u) && cpu.interrupts().read(0x343) == 0x2000 &&
            cpu.interrupts().read(0x341) == 64 && !bus.submits,
            "MMU user permission/PMP physical check ordering or virtual fault address differed");
    }
    // WARL MPP and MRET state changes, including clearing MPRV when dropping to U.
    State state;
    state.write(0x300, State::MPIE | State::MPRV);
    state.write(0x341, 64); require(state.mret() == 64 && state.privilege() == Privilege::User &&
        !(state.read(0x300) & (State::MPP | State::MPRV)) && (state.read(0x300) & State::MIE), "MRET stack rules failed");
    state.exception(64, 8, 0); require(state.privilege() == Privilege::Machine && !(state.read(0x300) & State::MPP),
        "trap did not record previous U privilege");
}
int main() {
    trap_cases(); memory_and_interrupts();
    std::cout << "PASS RV32 M/U: ECALL/MRET, forbidden CSRs/instructions, PMP fetch/data/MMIO checks, MPRV, IRQ priority and four CPU configurations\n";
}
