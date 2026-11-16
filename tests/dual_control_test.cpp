#include "rv32_test_support.hpp"

#include <array>
#include <random>

using namespace rv32_test;

#include "rv32_reference.hpp"

void test_retirement_and_oracle() {
    std::mt19937 random(0xC001C0DEu);
    std::vector<std::uint32_t> program{0x00A00093, 0x01400113, 0x002081B3,
        i(4, 0, -1), r(5, 4, 2), r(5, 5, 5, true), sw(3, 31, -128), i(6, 31, -128, true)};
    for (unsigned index = 0; index < 1200; ++index) {
        const auto rd = random() % 31, rs1 = random() % 32, rs2 = random() % 32;
        const int immediate = static_cast<int>(random() % 4096) - 2048;
        const int offset = (static_cast<int>(random() % 33) - 16) * 4;
        switch (random() % 6) {
        case 0: program.push_back(r(rd, rs1, rs2)); break;
        case 1: program.push_back(r(rd, rs1, rs2, true)); break;
        case 2: program.push_back(i(rd, rs1, immediate)); break;
        case 3: program.push_back(i(rd, 31, offset, true)); break;
        case 4: program.push_back(sw(rs2, 31, offset)); break;
        case 5: program.push_back(beq(rs1, random() % 2 ? rs1 : rs2, 8)); break;
        }
    }
    program.push_back(i(0, 0, 0)); // 末尾跳过一条也只能到达程序边界。
    CPU hard(program, Mode::Hardwired, 64), micro(program, Mode::Microprogrammed, 64);
    Reference reference;
    for (unsigned reg = 1; reg < 32; ++reg) {
        const auto value = reg == 31 ? 128u : random();
        hard.set_register(reg, value); micro.set_register(reg, value); reference.registers[reg] = value;
    }
    for (unsigned address = 0; address < 256; address += 4) {
        const auto value = random();
        hard.set_memory(address, value); micro.set_memory(address, value); reference.memory[address / 4] = value;
    }
    while (reference.pc / 4 < program.size()) {
        const auto word = program[reference.pc / 4];
        reference.step(word);
        const auto a = hard.step_instruction(), b = micro.step_instruction();
        assert(a.pc == b.pc && a.next_pc == b.next_pc && a.instruction == b.instruction && a.instruction == word);
        assert(a.instruction_number == b.instruction_number);
        assert(a.cycles < b.cycles);
        assert(bool(a.register_write) == bool(b.register_write));
        if (a.register_write) {
            assert(a.register_write->index == b.register_write->index);
            assert(a.register_write->value == b.register_write->value);
        }
        assert(bool(a.memory_write) == bool(b.memory_write));
        if (a.memory_write) {
            assert(a.memory_write->address == b.memory_write->address);
            assert(a.memory_write->value == b.memory_write->value);
        }
        compare_architecture(hard, micro);
        compare_to_reference(hard, reference);
    }
    assert(hard.completed_instructions() > 900);
}

void test_backward_branch_and_wrap() {
    const std::vector<std::uint32_t> loop{i(1, 0, 3), i(1, 1, -1), beq(1, 0, 8), beq(0, 0, -8), i(2, 1, 9)};
    CPU hard(loop), micro(loop, Mode::Microprogrammed);
    for (int n = 0; n < 10; ++n) {
        hard.step_instruction(); micro.step_instruction(); compare_architecture(hard, micro);
    }
    assert(hard.registers().read(1) == 0 && hard.registers().read(2) == 9 && hard.pc() == 20);
    for (auto mode : {Mode::Hardwired, Mode::Microprogrammed}) {
        CPU cpu({i(1, 1, 1), r(2, 2, 3), r(3, 0, 3, true), i(0, 0, 1),
                 sw(2, 4, 4), i(5, 4, 4, true)}, mode);
        cpu.set_register(0, 123);
        cpu.set_register(1, 0x7FFFFFFF); cpu.set_register(2, 0xFFFFFFFF); cpu.set_register(3, 1);
        cpu.set_register(4, 0xFFFFFFFC);
        cpu.run_instructions(6);
        assert(cpu.registers().read(0) == 0 && cpu.registers().read(1) == 0x80000000);
        assert(cpu.registers().read(2) == 0 && cpu.registers().read(3) == 0xFFFFFFFF);
        assert(cpu.memory().read(0) == 0 && cpu.registers().read(5) == 0);
    }
}

struct FaultRecord { Cause cause; std::uint32_t pc, value; };
FaultRecord fault(CPU& cpu) {
    for (unsigned beat = 0; beat < 64; ++beat) {
        try { cpu.tick(); }
        catch (const Fault& error) {
            const auto pulses = cpu.clock_pulses();
            const auto retired = cpu.completed_instructions();
            const auto pc = cpu.pc();
            const auto saved_state = cpu.state();
            const FaultRecord record{error.cause(), error.pc(), error.value()};
            for (int retry = 0; retry < 2; ++retry) {
                bool threw = false;
                try { cpu.tick(); }
                catch (const Fault& again) {
                    threw = true;
                    assert(again.cause() == record.cause && again.pc() == record.pc && again.value() == record.value);
                }
                assert(threw && cpu.clock_pulses() == pulses && cpu.completed_instructions() == retired && cpu.pc() == pc);
                assert(cpu.state().ir == saved_state.ir && cpu.state().mdr == saved_state.mdr && cpu.state().mar == saved_state.mar);
            }
            return record;
        }
    }
    assert(false); return {};
}
void fault_pair(std::vector<std::uint32_t> program, std::uint32_t base, Cause cause,
                std::uint32_t fault_pc, std::uint32_t value) {
    CPU hard(program, Mode::Hardwired, 16), micro(program, Mode::Microprogrammed, 16);
    for (auto* cpu : {&hard, &micro}) {
        cpu->set_register(1, base); cpu->set_register(2, 99); cpu->set_register(3, 12345);
        cpu->set_memory(0, 0xCAFEBABE);
    }
    const auto a = fault(hard), b = fault(micro);
    assert(a.cause == cause && a.pc == fault_pc && a.value == value);
    assert(a.cause == b.cause && a.pc == b.pc && a.value == b.value);
    compare_architecture(hard, micro);
    assert(hard.registers().read(3) == 12345 && hard.memory().read(0) == 0xCAFEBABE);
}
void test_faults() {
    fault_pair({}, 0, Cause::InstructionAccessFault, 0, 0);
    fault_pair({0}, 0, Cause::IllegalInstruction, 0, 0);
    fault_pair({i(4, 0, 7), 0xFFFFFFFF}, 0, Cause::IllegalInstruction, 4, 0xFFFFFFFF);
    fault_pair({i(3, 1, 0, true)}, 2, Cause::LoadAddressMisaligned, 0, 2);
    fault_pair({i(0, 1, 0, true)}, 2, Cause::LoadAddressMisaligned, 0, 2); // LW x0 仍须访存/报错。
    fault_pair({i(3, 1, 0, true)}, 64, Cause::LoadAccessFault, 0, 64);
    fault_pair({sw(2, 1, 0)}, 2, Cause::StoreAddressMisaligned, 0, 2);
    fault_pair({sw(2, 1, 0)}, 0xFFFFFFFC, Cause::StoreAccessFault, 0, 0xFFFFFFFC);
    fault_pair({beq(1, 1, 2)}, 5, Cause::InstructionAddressMisaligned, 0, 2);
    fault_pair({beq(0, 0, -4)}, 5, Cause::InstructionAccessFault, 0xFFFFFFFC, 0xFFFFFFFC);
    for (auto mode : {Mode::Hardwired, Mode::Microprogrammed}) {
        CPU not_taken({beq(1, 2, 2)}, mode);
        not_taken.set_register(1, 1); not_taken.set_register(2, 2);
        not_taken.step_instruction();
        assert(not_taken.pc() == 4 && not_taken.completed_instructions() == 1);
    }
}

void test_mode_selection_and_signal_conflicts() {
    CPU cpu({i(1, 0, 1), i(2, 1, 2), r(3, 1, 2), sw(3, 0, 0), i(4, 0, 0, true)});
    cpu.tick();
    expect_throw<std::logic_error>([&] { cpu.select_mode(Mode::Microprogrammed); });
    assert(cpu.mode() == Mode::Hardwired && cpu.control_unit().microprogrammed().clock_pulses() == 0);
    assert(cpu.step_instruction().cycles == 10);
    cpu.select_mode(Mode::Microprogrammed);
    cpu.step_instruction();
    assert(cpu.control_unit().hardwired().clock_pulses() == 10);
    cpu.select_mode(Mode::Hardwired); cpu.step_instruction();
    assert(cpu.control_unit().microprogrammed().clock_pulses() == 11);
    cpu.select_mode(Mode::Microprogrammed); cpu.step_instruction();
    cpu.select_mode(Mode::Hardwired); cpu.step_instruction();
    assert(cpu.clock_pulses() == 57 && cpu.completed_instructions() == 5);
    assert(cpu.registers().read(3) == 4 && cpu.registers().read(4) == 4 && cpu.memory().read(0) == 4);
    expect_throw<std::invalid_argument>([&] { cpu.select_mode(static_cast<Mode>(42)); });

    using namespace colab::soc::core;
    datapath::RV32Datapath path;
    datapath::RV32State state;
    datapath::dedicated::InstructionMemory program({0x00A00093});
    datapath::dedicated::DataMemory memory;
    for (int conflict = 0; conflict < 6; ++conflict) {
        control_unit::rv32::ControlSignals signals;
        switch (conflict) {
        case 0: signals.instruction_read = true; signals.data_read = true; break;
        case 1: signals.data_read = true; signals.memory_write = true; break;
        case 2: signals.instruction_to_mdr = true; break;
        case 3: signals.data_to_mdr = true; break;
        case 4: signals.register_write = true; break;
        case 5: signals.mar_from_pc = true; signals.address_in = true; break;
        }
        expect_throw<std::logic_error>([&] { path.evaluate(state, {}, signals, program, memory); });
    }
}

int main() {
    test_retirement_and_oracle();
    test_backward_branch_and_wrap();
    test_faults();
    test_mode_selection_and_signal_conflicts();
}
