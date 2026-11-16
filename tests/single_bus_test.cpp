#include "../soc/core/core.hpp"

#include <cassert>
#include <stdexcept>

using namespace colab::soc;
using namespace colab::soc::core;

template <typename Function>
void rejects(Function function) {
    bool threw = false;
    try { function(); } catch (const std::logic_error&) { threw = true; }
    assert(threw);
}

void test_tristate_bus() {
    NoC noc;
    Registers registers;
    registers.pc.write(23);
    registers.mdr.write(23); // 即使驱动相同数值，多输出仍冲突。
    registers.gprs.write(15, 0xABCD);
    assert(noc.read(BusDrivers{true, false, false, false}, registers) == 23);
    rejects([&] { noc.read(BusDrivers{true, true, false, false}, registers); });
    assert(!noc.read(BusDrivers{}, registers)); // 不保留上一拍。
    assert(noc.read(BusSource::GPR, registers, 15) == 0xABCD);
}

void test_inputs_and_system_bus() {
    datapath::SingleBusDatapath path;
    Registers registers;
    registers.pc.write(42);
    control_unit::ControlSignals signals;
    signals.mar_in = true;
    rejects([&] { path.evaluate(registers, signals); });
    signals.bus_source = BusSource::PC;
    signals.mdr_in = true;
    signals.y_in = true;
    const auto fanout = path.evaluate(registers, signals);
    assert(registers.mar.read() == 0); // evaluate 不提前锁存。
    assert(fanout.next_registers.mar.read() == 42);
    assert(fanout.next_registers.mdr.read() == 42);
    assert(fanout.next_registers.y.read() == 42);
    registers = fanout.next_registers;
    signals = {};
    signals.write = true;
    const auto store = path.evaluate(registers, signals);
    assert(!store.bus && store.memory_request->write && !store.memory_request->read);
    assert(store.memory_request->address == 42 && store.memory_request->data == 42);
    signals.read = true;
    rejects([&] { path.evaluate(registers, signals); });
    signals = {};
    signals.read = signals.mdr_in = signals.memory_to_mdr = true;
    rejects([&] { path.evaluate(registers, signals); });
    const auto load = path.evaluate(registers, signals, 0, 0, 0x1234);
    assert(!load.bus && load.next_registers.mdr.read() == 0x1234);
    registers = load.next_registers;
    signals = {};
    signals.bus_source = BusSource::MDR;
    signals.pc_in = true;
    assert(path.evaluate(registers, signals).next_registers.pc.read() == 0x1234);
    signals.increment_pc = true;
    rejects([&] { path.evaluate(registers, signals); });
}

void test_figure_5_6() {
    // 教材：(rs)+(rd) -> rd；rs=0001，rd=0010。
    Core cpu({{colab::isa::Opcode::Add, 2, 1, 2}});
    cpu.set_register(1, 17);
    cpu.set_register(2, 25);
    for (unsigned c = 1; c <= 10; ++c) {
        cpu.tick();
        const auto& trace = *cpu.last_cycle();
        assert(trace.timing.instruction_beat == c);
        assert(trace.signals.read == (c >= 2 && c <= 6));
        assert(trace.signals.mdr_in == (c >= 2 && c <= 6));
        assert(trace.instruction_complete == (c == 10));
        if (c == 1) assert(trace.signals.mar_in && trace.signals.bus_source == BusSource::PC);
        if (c >= 2 && c <= 6) assert(!trace.bus);
        if (c < 6) assert(cpu.registers().mdr.read() == 0);
        if (c == 6) assert(cpu.registers().mdr.read() == 0x1212);
        if (c < 7) assert(cpu.registers().ir.read() == 0);
        if (c == 7) assert(trace.signals.ir_in && trace.bus == 0x1212);
        if (c == 8) assert(trace.signals.y_in && trace.bus == 17 && cpu.registers().y.read() == 17);
        if (c == 9) assert(trace.alu->value == 42 && cpu.registers().z.read() == 42);
        if (c < 10) assert(cpu.registers().gprs.read(2) == 25);
        if (c == 10) assert(trace.signals.gpr_in && trace.bus == 42 && cpu.registers().gprs.read(2) == 42);
    }
}

int main() {
    test_tristate_bus();
    test_inputs_and_system_bus();
    test_figure_5_6();
}
