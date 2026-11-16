#include "rv32_test_support.hpp"

using namespace rv32_test;
using namespace colab::soc::core;

int main() {
    CPU cpu({0x00A00093, 0x01400113, 0x002081B3});
    for (unsigned beat = 1; beat <= 10; ++beat) {
        cpu.tick();
        const auto& trace = *cpu.last_cycle();
        assert(trace.mode == Mode::Hardwired && !trace.micro_address);
        assert(trace.timing.clock_pulse == beat && trace.timing.instruction_beat == beat);
        assert(trace.instruction_complete == (beat == 10));
        assert(cpu.registers().read(1) == (beat == 10 ? 10u : 0u));
        assert(cpu.pc() == (beat == 10 ? 4u : 0u));
        assert(trace.signals.mar_from_pc == (beat == 1));
        assert(trace.signals.instruction_read == (beat >= 2 && beat <= 6));
        assert(trace.signals.instruction_to_mdr == (beat == 6));
        assert(trace.signals.ir_in == (beat == 7));
        assert(trace.signals.operands_in == (beat == 8));
        assert(trace.signals.alu_in == (beat == 9));
        if (beat < 6) assert(cpu.state().mdr == 0);
        if (beat == 6) assert(cpu.state().mdr == 0x00A00093 && cpu.state().ir == 0);
        if (beat == 7) assert(cpu.state().ir == 0x00A00093);
        if (beat == 9) assert(cpu.state().alu_out == 10);
    }
    cpu.run_instructions(2);
    assert(cpu.registers().read(3) == 30 && cpu.clock_pulses() == 30);
    assert(cpu.control_unit().hardwired().clock_pulses() == 30);
    assert(cpu.control_unit().microprogrammed().clock_pulses() == 0);

    CPU memory({i(1, 0, 8), i(2, 0, -1), sw(2, 1, -8), i(3, 1, -8, true),
                r(4, 3, 2, true), beq(4, 0, 8), i(5, 0, 99), i(5, 0, 7)});
    const unsigned lengths[]{10, 10, 10, 15, 10, 10, 10};
    for (const auto length : lengths) assert(memory.step_instruction().cycles == length);
    assert(memory.registers().read(3) == 0xFFFFFFFFu && memory.registers().read(4) == 0);
    assert(memory.registers().read(5) == 7 && memory.memory().read(0) == 0xFFFFFFFFu);
    assert(memory.pc() == 32 && memory.clock_pulses() == 75);

    control_unit::HardwiredController controller;
    Decoder decoder;
    auto instruction = decoder.decode_rv32(r(3, 1, 2));
    for (int n = 0; n < 8; ++n) controller.clock(instruction, {});
    assert(controller.phase() == Phase::Execute);
    assert(controller.signals(instruction, {}).alu_in);
    controller.reset();
    assert(controller.phase() == Phase::FetchAddress && controller.clock_pulses() == 0);
}
