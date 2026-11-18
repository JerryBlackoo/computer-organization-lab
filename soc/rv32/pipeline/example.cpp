#include "../core.hpp"
#include <iostream>

int main() {
    using CPU = colab::soc::RV32Core;
    // addi x1,x0,10; sw x1,0(x0); lw x2,0(x0); add x3,x2,x1
    CPU cpu({0x00A00093, 0x00102023, 0x00002103, 0x001101B3}, CPU::Mode::Hardwired,
            256, {}, CPU::Execution::Pipelined);
    while (cpu.completed_instructions() < 4) {
        cpu.tick();
        const auto& trace = *cpu.last_pipeline_cycle();
        std::cout << "cycle " << trace.pulse;
        const char* names[]{"IF", "ID", "EX", "MEM", "WB"};
        for (unsigned n = 0; n < 5; ++n) {
            std::cout << ' ' << names[n] << '=';
            if (trace.pc[n]) std::cout << *trace.pc[n]; else std::cout << '-';
        }
        if (trace.data_stall) std::cout << " load-use bubble";
        if (trace.retirement) std::cout << " retire=" << trace.retirement->pc;
        std::cout << '\n';
    }
    std::cout << "x3=" << cpu.registers().read(3) << " cycles=" << cpu.clock_pulses()
              << " retired=" << cpu.completed_instructions() << '\n';
    return cpu.registers().read(3) != 20 || cpu.clock_pulses() != 9;
}
