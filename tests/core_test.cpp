#include "../simulation/simulator.hpp"
#include "../soc/cpu16/core.hpp"

#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <iostream>

using colab::isa::Opcode;
using colab::soc::Core;
using colab::soc::core::datapath::BusSource;
using colab::soc::core::control_unit::Phase;

template <typename Exception, typename Function>
void expect_throw(Function function) {
    bool threw = false;
    try { function(); }
    catch (const Exception&) { threw = true; }
    assert(threw);
}

static bool same_flags(colab::soc::core::registers::Flags a, colab::soc::core::registers::Flags b) {
    return a.sf == b.sf && a.of == b.of && a.cf == b.cf && a.zf == b.zf;
}
static void flag_lifecycle() {
    using colab::soc::core::registers::Flags;
    using colab::soc::core::registers::FR;
    const Flags clear{};
    FR fr;
    assert(same_flags(fr.read(), clear));
    fr.write({true, true, true, true});
    auto copy = fr.read(); copy.cf = false; copy.zf = false;
    assert(!copy.cf && !copy.zf && fr.read().cf && fr.read().zf);
    fr.write({}); assert(same_flags(fr.read(), clear));
    // Z may update without FR; evaluate computes next state but must not mutate the old state.
    colab::soc::core::Registers registers;
    registers.fr.write({true, true, true, true}); registers.y.write(1); registers.gprs.write(2, 1);
    colab::soc::core::control_unit::ControlSignals signals;
    signals.z_in = true; signals.bus_source = BusSource::GPR;
    colab::soc::core::datapath::SingleBusDatapath datapath;
    const auto held = datapath.evaluate(registers, signals, 2);
    assert(held.next_registers.z.read() == 2 && same_flags(held.next_registers.fr.read(), registers.fr.read()));
    signals.fr_in = true;
    const auto updated = datapath.evaluate(registers, signals, 2);
    assert(same_flags(updated.next_registers.fr.read(), clear));
    assert(registers.fr.read().cf && registers.fr.read().zf); // Still no clock commit.

    struct Case { Opcode op; std::uint16_t a, b; Flags flags; };
    const Case cases[] = {
        {Opcode::Add, 0, 0, {false,false,false,true}},
        {Opcode::Add, 0xFFFF, 1, {false,false,true,true}},
        {Opcode::Add, 0x7FFF, 1, {true,true,false,false}},
        {Opcode::Add, 0x8000, 0x8000, {false,true,true,true}},
        {Opcode::Add, 0xFFFF, 0, {true,false,false,false}},
        {Opcode::Sub, 0, 1, {true,false,true,false}},
        {Opcode::Sub, 0xFFFF, 1, {true,false,false,false}},
        {Opcode::Sub, 0x7FFF, 0xFFFF, {true,true,true,false}},
        {Opcode::Sub, 0x8000, 1, {false,true,false,false}},
        {Opcode::Sub, 0x8000, 0x8000, {false,false,false,true}}
    };
    for (const auto& test : cases) {
        Core cpu({{test.op, 3, 1, 2}, {Opcode::Halt}});
        cpu.set_register(1,test.a);cpu.set_register(2,test.b);cpu.set_register(3,0x1234);
        assert(same_flags(cpu.registers().fr.read(), clear));
        for (unsigned i=0;i<8;++i) {cpu.tick();assert(same_flags(cpu.registers().fr.read(),clear));}
        assert(cpu.phase() == Phase::Execute);
        cpu.tick(); // Execute edge writes all four flags, but not the destination GPR yet.
        assert(same_flags(cpu.registers().fr.read(),test.flags) && cpu.registers().gprs.read(3)==0x1234);
        assert(same_flags(cpu.last_cycle()->alu->flags,test.flags));
        cpu.tick();assert(same_flags(cpu.registers().fr.read(),test.flags)); // Writeback.
        for (unsigned i=0;i<8;++i) {cpu.tick();assert(same_flags(cpu.registers().fr.read(),test.flags));}
        assert(cpu.is_halted());cpu.tick();assert(same_flags(cpu.registers().fr.read(),test.flags));
    }
}
int main() {
    flag_lifecycle();
    Core core({{Opcode::Add, 3, 1, 2}, {Opcode::Sub, 3, 3, 2},
               {Opcode::Add, 0, 0, 0}, {Opcode::Halt}});
    core.set_register(0, 0xFFFF); // 教学 ISA 中 R0 是普通寄存器。
    core.set_register(1, 0x7FFF);
    core.set_register(2, 1);
    core.set_register(3, 0x1234);
    core.set_register(15, 0xABCD);
    assert(core.registers().gprs.read(15) == 0xABCD);
    assert(!core.last_cycle());
    colab::simulation::Simulator<Core> simulator(core);

    simulator.step(); // PCout, MARin
    assert(core.phase() == Phase::FetchRead);
    assert(core.registers().mar.read() == 0);
    assert(core.last_cycle()->signals.bus_source == BusSource::PC);
    assert(core.last_cycle()->bus.value() == 0);
    simulator.run(4); // Read，等待至第五拍结束。
    assert(core.registers().mdr.read() == 0);
    assert(core.registers().ir.read() == 0);
    assert(!core.last_cycle()->bus); // 等待拍不沿用上一拍的总线数据。
    simulator.step(); // Memory -> MDR
    assert(core.registers().mdr.read() == 0x1312);
    assert(core.registers().ir.read() == 0);
    simulator.step(); // MDRout, IRin
    assert(simulator.cycles() == 7);
    assert(core.registers().ir.read() == 0x1312);
    assert(core.last_cycle()->bus.value() == 0x1312);
    assert(core.registers().pc.read() == 2);
    assert(core.phase() == Phase::ReadOperand);
    assert(core.last_cycle()->signals.decode && core.last_cycle()->signals.ir_in);
    assert(core.registers().y.read() == 0); // C8 才读第一个操作数。
    simulator.step(); // GPR[rs1] -> Y
    assert(core.registers().y.read() == 0x7FFF);
    assert(core.last_cycle()->bus.value() == 0x7FFF);
    assert(core.registers().gprs.read(3) == 0x1234);
    assert(!core.registers().fr.read().sf && !core.registers().fr.read().of);
    simulator.step(); // Y + GPR[rs2] -> Z, FR
    assert(core.registers().z.read() == 0x8000);
    assert(core.last_cycle()->bus.value() == 1); // 第二个操作数来自 rs2。
    assert(core.registers().fr.read().sf && core.registers().fr.read().of);
    assert(core.registers().gprs.read(3) == 0x1234); // 写回发生在下一拍。
    simulator.step(); // Z -> GPR[rd]
    assert(core.registers().gprs.read(3) == 0x8000);
    assert(core.last_cycle()->bus.value() == 0x8000); // Z 经总线写回。
    assert(simulator.cycles() == 10);

    simulator.run(8); // 第二条指令取指兼译码、保存第一个操作数。
    assert(core.registers().y.read() == 0x8000);
    assert(core.registers().fr.read().sf && core.registers().fr.read().of);
    simulator.step(); // 0x8000 - 1 -> 0x7FFF
    assert(core.registers().z.read() == 0x7FFF);
    assert(!core.registers().fr.read().sf && core.registers().fr.read().of);
    assert(core.registers().gprs.read(3) == 0x8000);
    simulator.step();
    assert(core.registers().gprs.read(3) == 0x7FFF);

    simulator.run(10); // 连续运算正确使用旧 R0，且清除之前的 OF。
    assert(core.registers().gprs.read(0) == 0xFFFE);
    assert(core.registers().fr.read().sf && !core.registers().fr.read().of);
    simulator.run(8); // HALT 不改变 FR。
    assert(core.is_halted());
    assert(simulator.cycles() == 38);
    assert(core.registers().fr.read().sf && !core.registers().fr.read().of);
    core.tick();
    assert(core.registers().pc.read() == 8);
    assert(core.last_cycle()->phase == Phase::Halt);

    expect_throw<std::out_of_range>([&] { core.set_register(16, 1); });
    expect_throw<std::out_of_range>([] { Core invalid({{Opcode::Add, 16, 1, 2}}); });

    Core empty({});
    colab::simulation::Simulator<Core> empty_simulator(empty);
    empty_simulator.step();
    expect_throw<std::out_of_range>([&] { empty_simulator.step(); });
    assert(empty_simulator.cycles() == 1); // 失败的一拍没有被计入。
    assert(empty.phase() == Phase::FetchRead);
    assert(empty.registers().pc.read() == 0);
    assert(empty.last_cycle()->phase == Phase::FetchAddress);

    // 无 HALT 的程序执行到边界时明确报错，不隐式读取零指令。
    Core unterminated({{Opcode::Add, 3, 1, 2}});
    colab::simulation::Simulator<Core> short_simulator(unterminated);
    short_simulator.run(11);
    expect_throw<std::out_of_range>([&] { short_simulator.step(); });
    assert(short_simulator.cycles() == 11);
    assert(unterminated.registers().mar.read() == 2);

    colab::soc::core::Decoder decoder;
    const auto instruction = decoder.decode(0x2345);
    assert(instruction.op == Opcode::Sub);
    assert(instruction.rd == 3 && instruction.rs1 == 4 && instruction.rs2 == 5);
    assert(colab::isa::encode(instruction) == 0x2345);
    expect_throw<std::invalid_argument>([&] { decoder.decode(0x0000); });
    expect_throw<std::invalid_argument>([&] { decoder.decode(0xF001); });
    std::cout << "PASS teaching Core: 10 four-flag Execute/writeback/HALT cases, FR hold/read-copy/default, original instruction timing\n";
}
