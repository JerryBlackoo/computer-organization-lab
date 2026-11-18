#include "../../soc/shared/wangdao09/execution.hpp"
#include "../../soc/rv32/core.hpp"
#include <iostream>

using W = colab::soc::core::wangdao09::Execution;
using CPU = colab::soc::RV32Core;
namespace pipe = colab::soc::core::pipeline;

const char* name(W::Phase phase) {
    switch (phase) {
    case W::Phase::IncInput: return "R1 -> bus -> Y";
    case W::Phase::IncAlu: return "Y -> ALU(inc) -> Z";
    case W::Phase::IncWrite: return "Z -> bus -> R1";
    case W::Phase::SubAddress: return "R2 -> bus -> MAR; bus -> memory address";
    case W::Phase::SubOperand: return "R3 -> bus -> Y; memory response -> MDR";
    case W::Phase::SubAlu: return "MDR -> bus; Y - bus -> Z";
    case W::Phase::SubWrite: return "Z -> bus -> R1";
    case W::Phase::Idle: break;
    }
    return "idle";
}
const char* forward(pipe::Forward f) {
    switch (f) {
    case pipe::Forward::Register: return "register";
    case pipe::Forward::ExMem: return "EX/MEM";
    case pipe::Forward::MemWb: return "MEM/WB";
    }
    return "?";
}
void demo16(bool bypass) {
    W cpu(bypass ? W::Bypass::Enabled : W::Bypass::Disabled);
    cpu.set_register(1, 5); cpu.set_register(2, 100); cpu.set_register(3, 20); cpu.set_memory16(100, 6);
    std::cout << "\n16-bit execution only; ALU input bypass " << (bypass ? "ON" : "OFF") << '\n';
    for (const auto word : {std::uint16_t{0x0240}, std::uint16_t{0x065e}}) {
        cpu.begin(word);
        std::cout << (word == 0x0240 ? "inc R1" : "sub R1,R3,(R2)") << '\n';
        while (!cpu.done()) {
            cpu.tick(); const auto& trace = *cpu.last_cycle();
            std::cout << "  T" << cpu.cycles() << ' ' << name(trace.phase);
            if (trace.signals.alu_a_from_bus) std::cout << "; bus -> ALU.A(inc) -> Z [bypass]";
            if (trace.bus) std::cout << " bus=" << *trace.bus;
            if (trace.alu) std::cout << " ALU=" << trace.alu->value;
            std::cout << '\n';
        }
        std::cout << "  execution cycles=" << cpu.cycles() << " R1=" << cpu.reg(1) << '\n';
    }
}
void demo32(bool forwarding) {
    // addi x2,x2,1; lhu x5,0(x3); sub x2,x4,x5
    CPU cpu({0x00110113, 0x0001d283, 0x40520133}, CPU::Mode::Hardwired,
            256, {}, CPU::Execution::Pipelined, {forwarding});
    cpu.set_register(2, 5); cpu.set_register(3, 100); cpu.set_register(4, 20); cpu.set_memory(100, 6);
    std::cout << "\nRV32 real pipeline; forwarding " << (forwarding ? "ON" : "OFF") << '\n';
    while (cpu.completed_instructions() < 3) {
        cpu.tick(); const auto& t = *cpu.last_pipeline_cycle();
        std::cout << "  T" << t.pulse << " EX=";
        if (t.pc[2]) std::cout << *t.pc[2]; else std::cout << '-';
        std::cout << " forward(A,B)=" << forward(t.forward_a) << ',' << forward(t.forward_b);
        if (t.data_stall) std::cout << " [data stall]";
        if (t.retirement) std::cout << " retire PC=" << t.retirement->pc;
        std::cout << '\n';
    }
    std::cout << "  total pipeline cycles=" << cpu.clock_pulses() << " x2=" << cpu.registers().read(2) << '\n';
}
int main() { demo16(false); demo16(true); demo32(false); demo32(true); }
