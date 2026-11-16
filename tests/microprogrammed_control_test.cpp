#include "rv32_test_support.hpp"

using namespace rv32_test;
using namespace colab::soc::core;
using namespace colab::soc::core::control_unit::microprogrammed;

int main() {
    CPU cpu({0x00A00093, 0x01400113, 0x002081B3}, Mode::Microprogrammed);
    const MicroAddress sequence[]{0, 1, 2, 3, 4, 5, 6, 7, 20, 21, 22};
    for (unsigned beat = 0; beat < 11; ++beat) {
        assert(cpu.control_unit().microprogrammed().micro_pc() == sequence[beat]);
        const auto& controller = cpu.control_unit().microprogrammed();
        assert(controller.micro_ir().phase == controller.control_store().at(sequence[beat]).phase);
        cpu.tick();
        assert(cpu.last_cycle()->micro_address == sequence[beat]);
        assert(cpu.clock_pulses() == beat + 1);
        assert(cpu.last_cycle()->instruction_complete == (beat == 10));
        assert(cpu.registers().read(1) == (beat == 10 ? 10u : 0u));
        assert(cpu.pc() == (beat == 10 ? 4u : 0u));
        assert(cpu.state().ir == (beat >= 6 ? 0x00A00093u : 0u));
    }
    assert(cpu.control_unit().microprogrammed().micro_pc() == 0);
    cpu.run_instructions(2);
    assert(cpu.registers().read(3) == 30 && cpu.clock_pulses() == 33);
    assert(cpu.control_unit().hardwired().clock_pulses() == 0);

    CPU memory({i(2, 0, 42), sw(2, 0, 4), i(3, 0, 4, true)}, Mode::Microprogrammed);
    assert(memory.step_instruction().cycles == 11);
    for (unsigned beat = 1; beat <= 11; ++beat) {
        memory.tick();
        assert(memory.memory().read(4) == (beat == 11 ? 42u : 0u));
        assert(memory.pc() == (beat == 11 ? 8u : 4u));
    }
    for (unsigned beat = 1; beat <= 16; ++beat) {
        memory.tick();
        assert(memory.last_cycle()->signals.data_read == (beat >= 11 && beat <= 15));
        assert(memory.last_cycle()->signals.data_to_mdr == (beat == 15));
        assert(memory.registers().read(3) == (beat == 16 ? 42u : 0u));
    }
    assert(memory.last_retirement()->cycles == 16 && memory.clock_pulses() == 38);

    for (bool equal : {false, true}) {
        CPU branch({beq(1, 2, 8), i(3, 0, 1), i(3, 0, 2)}, Mode::Microprogrammed);
        branch.set_register(1, 9); branch.set_register(2, equal ? 9 : 10);
        for (unsigned beat = 0; beat < 10; ++beat) branch.tick();
        assert(branch.control_unit().microprogrammed().micro_pc() == 42);
        assert(branch.state().equal == equal && branch.pc() == 0);
        branch.tick();
        assert(branch.control_unit().microprogrammed().micro_pc() == (equal ? 43 : 44));
        branch.tick();
        assert(branch.pc() == (equal ? 8u : 4u));
        assert(branch.last_retirement()->cycles == 12);
    }

    // 改 CM 中 ADD 的一条微指令即可变成减法，硬布线控制器的行为不受影响。
    ControlStore patched;
    auto word = patched.at(17);
    word.control.alu_op = datapath::dedicated::ALUCtr::Subu;
    patched.set(17, word);
    CPU micro({r(3, 1, 2)}, Mode::Microprogrammed, 16, patched);
    CPU hard({r(3, 1, 2)}, Mode::Hardwired, 16, patched);
    for (auto* core : {&micro, &hard}) {
        core->set_register(1, 20); core->set_register(2, 7); core->step_instruction();
    }
    assert(micro.registers().read(3) == 13 && hard.registers().read(3) == 27);

    // CM 错误下址既不推进控制器，也不提交尚未退休的机器指令。
    ControlStore bad;
    word = bad.at(22); word.next_address = 63; bad.set(22, word);
    CPU broken({i(1, 0, 5)}, Mode::Microprogrammed, 16, bad);
    for (unsigned beat = 0; beat < 10; ++beat) broken.tick();
    expect_throw<std::logic_error>([&] { broken.tick(); });
    assert(broken.pc() == 0 && broken.registers().read(1) == 0 && broken.clock_pulses() == 10);
    assert(broken.control_unit().microprogrammed().micro_pc() == 22);

    control_unit::MicroprogrammedController controller;
    Decoder decoder;
    controller.clock(decoder.decode_rv32(i(1, 0, 1)), {});
    controller.reset();
    assert(controller.micro_pc() == 0 && controller.clock_pulses() == 0);
    assert(controller.micro_ir().control.mar_from_pc);
    Microsequencer sequencer;
    Microinstruction jump; jump.sequence = Sequence::Jump; jump.next_address = 24;
    assert(sequencer.next(jump, {}, {}, patched) == 24);
    expect_throw<std::logic_error>([&] { patched.at(63); });
    expect_throw<std::out_of_range>([&] { patched.at(ControlStore::capacity); });
}
