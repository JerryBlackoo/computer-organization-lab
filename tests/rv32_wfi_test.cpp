#include "../soc/rv32/core.hpp"
#include <iostream>

using CPU = colab::soc::RV32Core;
using Interrupt = colab::soc::core::InterruptSystem;
using Privilege = colab::soc::core::PrivilegeMode;
static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static CPU cpu(std::vector<std::uint32_t> program, unsigned config) {
    return CPU(std::move(program), config == 1 ? CPU::Mode::Microprogrammed : CPU::Mode::Hardwired, 64, {},
        config >= 2 ? CPU::Execution::Pipelined : CPU::Execution::MultiCycle, {config != 3},
        colab::soc::core::FaultHandling::MachineTrap);
}
static void signal(Interrupt& state, unsigned cause, bool value) {
    if (cause == 11) state.set_external(value);
    else if (cause == 3) state.set_software(value);
    else state.set_timer(value);
}
int main() {
    for (unsigned config = 0; config < 4; ++config) for (unsigned cause : {11u, 3u, 7u})
        for (bool global : {false, true}) for (bool vectored : {false, true}) {
            const auto target = 64 + (vectored ? 4 * cause : 0);
            std::vector<std::uint32_t> program(64, 0x00000063);
            program[0] = 0x10500073; program[1] = 0x00700093; program[2] = 0x00202023;
            program[target / 4] = 0x342021f3; program[target / 4 + 1] = 0x30200073;
            auto machine = cpu(program, config);
            auto& state = machine.interrupts();
            state.write(0x305, 64 | unsigned(vectored)); state.write(0x304, 1u << cause); state.write(0x300, global ? 8 : 0);
            machine.set_register(2, 42); machine.set_memory(0, 99);
            const auto retired = machine.step_instruction(1000);
            require(retired.instruction == 0x10500073 && machine.pc() == 4 && machine.waiting() &&
                machine.completed_instructions() == 1 && !retired.register_write && !retired.memory_write,
                "WFI did not retire exactly once at next PC");
            const auto pulses = machine.clock_pulses();
            for (unsigned n = 0; n < 20; ++n) {
                machine.tick();
                require(machine.last_cycle()->wfi_wait && !machine.last_cycle()->bus_wait &&
                    !machine.last_cycle()->instruction_complete, "WFI idle clock was misclassified");
                if (config >= 2) require(machine.pipeline().empty() && !machine.last_pipeline_cycle()->pc[0],
                    "pipeline fetched or retained younger work while idle");
            }
            require(machine.clock_pulses() == pulses + 20 && machine.wfi_wait_cycles() == 20 && machine.pc() == 4 &&
                machine.registers().read(1) == 0 && machine.memory().read(0) == 99 && !machine.bus_waits(),
                "WFI stopped clocks or allowed younger effects");
            const auto other = cause == 11 ? 7u : 11u;
            signal(state, other, true); machine.tick(); machine.tick();
            require(machine.waiting() && machine.wfi_wait_cycles() == 22 && !state.entries(), "locally disabled interrupt woke WFI");
            signal(state, cause, true); machine.tick();
            require(!machine.waiting(), "locally enabled interrupt did not wake WFI");
            if (global) {
                require(machine.last_cycle()->interrupt_entry && state.read(0x341) == 4 && machine.pc() == target &&
                    state.read(0x342) == (0x80000000u | cause), "WFI wake took wrong trap/vector/resume PC");
                signal(state, cause, false); signal(state, other, false);
                machine.run_instructions(3, 1000);
                require(state.returns() == 1 && machine.registers().read(3) == (0x80000000u | cause), "IRQ handler/MRET failed");
            } else {
                for (unsigned n = 0; n < 1000 && machine.completed_instructions() < 2; ++n) machine.tick();
                require(!state.entries(), "globally disabled interrupt trapped instead of resuming");
            }
            require(machine.registers().read(1) == 7 && !state.exceptions(), "execution failed to resume after WFI");
        }
    for (unsigned config = 0; config < 4; ++config) {
        auto pending = cpu({0x10500073, 0x00700093}, config);
        pending.interrupts().write(0x304, Interrupt::MTIE); pending.interrupts().set_timer(true);
        pending.run_instructions(2, 1000);
        require(!pending.waiting() && !pending.wfi_wait_cycles() && pending.registers().read(1) == 7,
            "already-pending enabled interrupt incorrectly slept");
        auto wrong_path = cpu({0x00000463, 0x10500073, 0x00700093}, config);
        wrong_path.run_instructions(2, 1000);
        require(!wrong_path.waiting() && wrong_path.registers().read(1) == 7, "wrong-path WFI suspended the CPU");
        std::vector<std::uint32_t> user(64, 0x00000063); user[0] = 0x30200073; user[16] = 0x10500073;
        auto forbidden = cpu(user, config);
        auto& state = forbidden.interrupts(); state.write(0x3b0, 0x1fffffff); state.write(0x3a0, 0x1f);
        state.write(0x341, 64); state.write(0x305, 128); state.write(0x300, 0);
        for (unsigned n = 0; n < 1000 && !state.exceptions(); ++n) forbidden.tick();
        require(state.read(0x342) == 2 && state.read(0x341) == 64 && state.read(0x343) == 0x10500073 &&
            forbidden.completed_instructions() == 1 && !forbidden.waiting(), "U-mode WFI did not trap precisely");
    }
    Interrupt state; state.write(0x305, 0x41); state.write(0x304, 0xffffffff); state.write(0x300, 8);
    state.set_external(true); state.set_software(true); state.set_timer(true); state.write(0x344, 0);
    require(state.read(0x304) == 0xaaa && state.read(0x344) == 0x888, "local interrupt mask or hardware mip levels differed");
    for (unsigned cause : {11u, 3u, 7u}) {
        state.write(0x300, 8);
        require(state.enter(4) == 64 + 4 * cause && state.read(0x342) == (0x80000000u | cause), "MEI/MSI/MTI priority differed");
        signal(state, cause, false);
    }
    require(!state.requested(), "cleared local IRQ remained pending");
    std::cout << "PASS WFI/local IRQs: four CPU configurations, idle retirement, MIE-independent wake, vectors, priority and U-mode faults\n";
}
