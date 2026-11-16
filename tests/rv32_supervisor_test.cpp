#include "../soc/core/rv32_core.hpp"
#include <iostream>

using CPU = colab::soc::RV32Core;
using State = colab::soc::core::InterruptSystem;
using Privilege = colab::soc::core::PrivilegeMode;
using Access = colab::soc::core::mmu::AccessType;
static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Action> static void illegal(Action action) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("privileged operation unexpectedly permitted");
}
static std::uint32_t csr(unsigned number, unsigned rd, unsigned source = 0, bool write = false) {
    return (number << 20) | (source << 15) | (write ? 0x1000u : 0x2000u) | (rd << 7) | 0x73;
}
static CPU cpu(std::vector<std::uint32_t> code, unsigned config) {
    return CPU(std::move(code), config == 1 ? CPU::Mode::Microprogrammed : CPU::Mode::Hardwired, 256, {},
        config >= 2 ? CPU::Execution::Pipelined : CPU::Execution::MultiCycle, {config != 3},
        colab::soc::core::FaultHandling::MachineTrap);
}
static void prepare(State& state, Privilege mode) {
    state.write(0x3b0, 0x1fffffff); state.write(0x3a0, 0x1f); // PMP: whole 32-bit physical space, RWX.
    state.write(0x305, 384); state.write(0x105, 256 | 1u);
    state.write(0x341, 64); state.write(0x300, unsigned(mode) << 11);
}
template<class Predicate> static void until(CPU& machine, Predicate condition) {
    for (unsigned n = 0; n < 2000 && !condition(); ++n) machine.tick();
    require(condition(), "supervisor test timed out");
}
static void delegation_and_returns() {
    struct Case { Privilege mode; std::uint32_t word, cause, value, control; };
    const Case cases[]{
        {Privilege::Supervisor, 0x73, 9, 0, 0}, {Privilege::User, 0x73, 8, 0, 0},
        {Privilege::Supervisor, csr(0x305, 7), 2, csr(0x305, 7), 0},
        {Privilege::Supervisor, 0x30200073, 2, 0x30200073, 0},
        {Privilege::Supervisor, 0x10200073, 2, 0x10200073, State::TSR},
        {Privilege::Supervisor, 0x10500073, 2, 0x10500073, State::TW},
        {Privilege::Supervisor, csr(0x180, 7), 2, csr(0x180, 7), State::TVM},
        {Privilege::Supervisor, csr(0x180, 0, 0, true), 2, csr(0x180, 0, 0, true), State::TVM},
        {Privilege::Supervisor, 0x12f70073, 2, 0x12f70073, State::TVM},
        {Privilege::User, 0x10200073, 2, 0x10200073, 0},
        {Privilege::User, 0x12000073, 2, 0x12000073, 0},
        {Privilege::User, csr(0x100, 7), 2, csr(0x100, 7), 0},
        {Privilege::Supervisor, 0x0000a383, 5, 768, 0}, // Denied LW x7,0(x1).
        {Privilege::Supervisor, 0x0020a023, 7, 768, 0}, // Denied SW x2,0(x1).
    };
    for (unsigned config = 0; config < 4; ++config) for (bool delegated : {false, true}) for (const auto& item : cases) {
        std::vector<std::uint32_t> code(128, 0x00000063);
        code[0] = 0x30200073; code[16] = item.word; code[17] = 0x01f00213;
        const auto base = delegated ? 256u : 384u;
        const auto epc = delegated ? 0x141u : 0x341u;
        code[base / 4] = csr(delegated ? 0x142 : 0x342, 10);
        code[base / 4 + 1] = csr(epc, 11); code[base / 4 + 2] = 0x00458593;
        code[base / 4 + 3] = csr(epc, 0, 11, true);
        // M handler clears TSR before returning to S, S handler clears it through a test host below.
        code[base / 4 + 4] = delegated ? 0x10200073 : 0x30200073;
        auto machine = cpu(code, config); auto& state = machine.interrupts(); prepare(state, item.mode);
        if (item.cause == 5 || item.cause == 7) { state.write(0x3b0, 128); state.write(0x3a0, 0x0d); }
        state.write(0x302, delegated ? 1u << item.cause : 0);
        state.write(0x300, (unsigned(item.mode) << 11) | item.control | State::SIE);
        state.write(0x342, 0x1234); state.write(0x142, 0x5678);
        machine.set_register(1, 768); machine.set_register(2, 99); machine.set_register(7, 41);
        machine.set_memory(768, 42);
        until(machine, [&] { return state.exceptions() == 1; });
        require(machine.pc() == base && machine.completed_instructions() == 1 && state.read(epc) == 64 &&
            state.read(delegated ? 0x142 : 0x342) == item.cause && state.read(delegated ? 0x143 : 0x343) == item.value,
            "wrong delegated exception target/PC/cause/value/retirement");
        require(machine.registers().read(7) == 41 && machine.memory().read(768) == 42 && machine.registers().read(4) == 0,
            "fault or younger instruction had an architectural side effect");
        if (delegated) require(state.privilege() == Privilege::Supervisor && state.read(0x342) == 0x1234 &&
            bool(state.read(0x100) & State::SPP) == (item.mode == Privilege::Supervisor) &&
            !(state.read(0x100) & State::SIE) && (state.read(0x100) & State::SPIE), "S trap damaged M bank or status stack");
        else require(state.privilege() == Privilege::Machine && state.read(0x142) == 0x5678 &&
            ((state.read(0x300) & State::MPP) >> 11) == unsigned(item.mode), "M trap damaged S bank or saved wrong privilege");
        state.write(0x300, state.read(0x300) & ~State::TSR);
        until(machine, [&] { return machine.registers().read(4) == 31; });
        require(state.privilege() == item.mode && state.exceptions() == 1 && state.returns() == 2 &&
            machine.registers().read(10) == item.cause, "xRET failed to restore original privilege/next PC");
    }
}
static void irq_and_wfi() {
    for (unsigned config = 0; config < 4; ++config) for (unsigned cause : {1u, 5u, 9u})
        for (bool global : {false, true}) for (bool vector : {false, true}) {
            const auto bit = 1u << cause, target = 256 + (vector ? 4 * cause : 0u);
            std::vector<std::uint32_t> code(128, 0x00000063);
            code[0] = 0x30200073; code[16] = 0x10500073; code[17] = 0x01f00213;
            code[target / 4] = 0x10200073;
            auto machine = cpu(code, config); auto& state = machine.interrupts(); prepare(state, Privilege::Supervisor);
            state.write(0x105, 256 | unsigned(vector)); state.write(0x303, bit); state.write(0x304, bit);
            state.write(0x300, (1u << 11) | (global ? State::SIE : 0u));
            until(machine, [&] { return machine.waiting(); });
            require(machine.completed_instructions() == 2 && machine.pc() == 68, "S-mode WFI did not retire/flush");
            for (unsigned n = 0; n < 10; ++n) machine.tick();
            require(machine.wfi_wait_cycles() == 10 && machine.registers().read(4) == 0, "S WFI did not suspend younger instructions");
            state.write(0x344, bit); machine.tick();
            require(!machine.waiting() && state.entries() == unsigned(global), "SIE-independent WFI wake failed");
            if (global) require(machine.pc() == target && state.read(0x141) == 68 &&
                state.read(0x142) == (0x80000000u | cause) && (state.read(0x100) & State::SPP), "wrong S interrupt bank/vector/PC");
            state.write(0x344, 0);
            until(machine, [&] { return machine.registers().read(4) == 31; });
            require(state.privilege() == Privilege::Supervisor && !state.exceptions(), "S WFI did not resume in S-mode");
        }
}
static void state_and_csr_rules() {
    State state; prepare(state, Privilege::Supervisor);
    state.write(0x302, 0xffffffff); state.write(0x303, 0xffffffff);
    require(state.read(0x302) == 0xb3ff && state.read(0x303) == 0x222, "delegation WARL mask accepted machine/reserved causes");
    state.write(0x300, State::MPP | State::MIE | State::MPRV | State::TVM | State::TW | State::TSR);
    state.write(0x100, 0xffffffff);
    require((state.read(0x300) & (State::MPP | State::MIE | State::MPRV | State::TVM | State::TW | State::TSR)) ==
        (State::MPP | State::MIE | State::MPRV | State::TVM | State::TW | State::TSR) && state.read(0x100) == (0x122 | State::SUM | State::MXR),
        "sstatus alias changed machine-only bits or retained unsupported fields");
    state.write(0x304, 0xffffffff); state.write(0x104, 0);
    require(state.read(0x304) == 0x888 && !state.read(0x104), "sie write changed machine enables");
    state.write(0x344, 0xffffffff); state.set_external(true); state.write(0x144, 0);
    require(state.read(0x344) == 0xa20 && state.read(0x144) == 0x220, "sip write cleared hardware/firmware-only inputs");
    state.write(0x303, State::SSIE); state.write(0x144, 0xffffffff);
    require(state.read(0x144) == 2 && state.read(0x104) == 0, "S aliases exposed undelegated interrupt bits");
    for (unsigned value : {0u, 0xffffffffu, 0x80012345u}) {
        state.write(0x180, value); require(state.read(0x180) == ((value & 0x80000000u) ? value : 0u), "satp WARL mode/ASID/PPN mismatch");
    }
    state.write(0x141, 0x47); state.write(0x105, 0x103);
    require(state.read(0x141) == 0x44 && state.read(0x105) == 0x100, "S CSR alignment/vector WARL failed");
    state.write(0x300, State::MPRV | (1u << 11));
    require(state.data_privilege() == Privilege::Supervisor, "MPRV did not select S PMP privilege");
    state.write(0x300, 2u << 11); require(!(state.read(0x300) & State::MPP), "reserved MPP was retained");

    // Nested U -> S -> M traps must preserve the S bank and return through both stacks.
    State nested; prepare(nested, Privilege::User); nested.write(0x302, 1u << 8);
    nested.write(0x300, State::SIE | State::MPRV); nested.mret();
    require(nested.exception(64, 8, 0) == 256 && nested.privilege() == Privilege::Supervisor, "U exception was not delegated");
    nested.exception(268, 9, 0);
    require(nested.read(0x141) == 64 && nested.read(0x142) == 8 && nested.read(0x341) == 268 &&
        (nested.read(0x300) & State::MPP) == (1u << 11), "nested S ECALL overwrote S context");
    require(nested.mret() == 268 && nested.privilege() == Privilege::Supervisor && nested.sret() == 64 &&
        nested.privilege() == Privilege::User && (nested.read(0x100) & State::SIE) &&
        !(nested.read(0x300) & (State::MPP | State::SPP | State::MPRV)), "nested xRET failed");
    State machine; machine.write(0x302, 0xffffffff); machine.write(0x305, 384);
    require(machine.exception(4, 2, 0) == 384 && machine.privilege() == Privilege::Machine,
        "M exception was delegated downward");
    // M-mode may execute SRET even with TSR set; SRET always clears MPRV.
    machine.write(0x300, State::TSR | State::MPRV | State::SPP | State::SPIE);
    machine.write(0x141, 64); require(machine.sret() == 64 && machine.privilege() == Privilege::Supervisor &&
        !(machine.read(0x300) & State::MPRV), "M-mode SRET/TSR/MPRV rules failed");
    illegal([&] { machine.sret(); });
}
static void irq_privilege_and_priority() {
    for (auto mode : {Privilege::User, Privilege::Supervisor, Privilege::Machine})
        for (bool delegated : {false, true}) for (bool global : {false, true}) for (unsigned cause : {1u, 5u, 9u}) {
            State state; prepare(state, mode); const auto bit = 1u << cause;
            state.write(0x303, delegated ? bit : 0); state.write(0x304, bit); state.write(0x344, bit);
            state.write(0x300, (unsigned(mode) << 11) | (global ? State::SIE | State::MIE : 0u));
            if (mode != Privilege::Machine) state.mret();
            // MRET restores MIE from MPIE=0; lower privilege ignores this bit.
            const bool expected = delegated ? mode == Privilege::User || (mode == Privilege::Supervisor && global) :
                mode != Privilege::Machine || global;
            require(state.requested() == expected && state.wake_pending(), "delegation/global privilege IRQ mask failed");
            if (expected) {
                const auto target = state.enter(64);
                require(target == (delegated ? 256 + 4 * cause : 384) &&
                    state.privilege() == (delegated ? Privilege::Supervisor : Privilege::Machine), "wrong IRQ privilege destination");
            }
        }
    State priority; prepare(priority, Privilege::Supervisor); priority.write(0x303, State::SupervisorEnables);
    priority.write(0x304, 0xaaa); priority.write(0x344, 0x222); priority.set_timer(true);
    priority.write(0x300, (1u << 11) | State::SIE); priority.mret();
    require(priority.enter(64) == 384 && priority.read(0x342) == 0x80000007, "M-target IRQ did not precede delegated SEI");
    priority.set_timer(false); priority.mret();
    for (unsigned cause : {9u, 1u, 5u}) {
        require(priority.enter(64) == 256 + 4 * cause && priority.read(0x142) == (0x80000000u | cause), "SEI/SSI/STI priority failed");
        priority.write(0x344, priority.read(0x344) & ~(1u << cause)); priority.sret();
    }
}
static void fences_and_wrong_paths() {
    for (unsigned config = 0; config < 4; ++config) {
        std::vector<std::uint32_t> code(128, 0x00000063); code[0] = 0x30200073;
        code[16] = 0x12f70073; code[17] = 0x01f00213;
        auto machine = cpu(code, config); prepare(machine.interrupts(), Privilege::Supervisor);
        machine.mmu().enable_paging(0, [](auto) -> colab::soc::core::mmu::PageTableEntry {
            return {0, true, {true, true, true, true}, true, true, 0};
        });
        machine.mmu().translate(0);
        require(machine.mmu().tlb().entries()[0].valid, "SFENCE fixture did not populate TLB");
        machine.run_instructions(2, 2000);
        require(std::none_of(machine.mmu().tlb().entries().begin(), machine.mmu().tlb().entries().end(),
            [](const auto& entry) { return entry.valid; }) && !machine.interrupts().exceptions(), "SFENCE.VMA did not flush TLB");
        machine.run_instructions(1, 1000); require(machine.registers().read(4) == 31, "SFENCE wrote a register or failed to resume");
        for (auto wrong : {0x10200073u, 0x12000073u}) {
            auto branch = cpu({0x00000463, wrong, 0x01f00213}, config);
            branch.run_instructions(2, 1000);
            require(branch.interrupts().privilege() == Privilege::Machine && !branch.interrupts().returns() &&
                branch.registers().read(4) == 31 && !branch.interrupts().exceptions(), "wrong-path privileged instruction committed");
        }
    }
}
int main() {
    delegation_and_returns(); irq_and_wfi(); state_and_csr_rules(); irq_privilege_and_priority(); fences_and_wrong_paths();
    std::cout << "PASS S-mode: delegated/nested traps, separate banks, SRET, PMP, CSR aliases, TVM/TW/TSR, IRQ priority/masks, WFI and SFENCE.VMA in four CPU configurations\n";
}
