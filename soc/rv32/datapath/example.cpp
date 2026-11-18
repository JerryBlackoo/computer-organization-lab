#include "../core.hpp"
#include <iostream>

int main() {
    using CPU = colab::soc::RV32Core;
    const std::vector<std::uint32_t> program{0x00A00093, 0x01400113, 0x002081B3};
    for (const auto execution : {CPU::Execution::MultiCycle, CPU::Execution::Pipelined}) {
        CPU cpu(program, CPU::Mode::Hardwired, 256, {}, execution);
        cpu.run_instructions(3);
        std::cout << (execution == CPU::Execution::Pipelined ? "RV32 pipeline" : "RV32 multi-cycle")
                  << ": x3=" << cpu.registers().read(3) << " cycles=" << cpu.clock_pulses() << '\n';
        if (cpu.registers().read(3) != 30) return 1;
    }
}
