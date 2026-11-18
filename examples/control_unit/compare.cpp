#include "../../soc/rv32/core.hpp"

#include <iostream>

int main() {
    using CPU = colab::soc::RV32Core;
    // addi x1,x0,10; addi x2,x0,20; add x3,x1,x2
    const std::vector<std::uint32_t> program{0x00A00093, 0x01400113, 0x002081B3};
    for (const auto mode : {CPU::Mode::Hardwired, CPU::Mode::Microprogrammed, CPU::Mode::VerticalMicroprogrammed}) {
        CPU cpu(program, mode);
        const auto name = mode == CPU::Mode::Hardwired ? "Hardwired" :
            mode == CPU::Mode::Microprogrammed ? "Horizontal" : "Vertical";
        std::cout << name << '\n';
        for (unsigned index = 0; index < 3; ++index) {
            const auto trace = cpu.step_instruction();
            std::cout << "  retire " << trace.instruction_number << " PC=" << trace.pc
                      << " -> " << trace.next_pc << " cycles=" << trace.cycles << '\n';
        }
        std::cout << "  x3=" << cpu.registers().read(3) << " total_cycles=" << cpu.clock_pulses() << '\n';
        if (cpu.registers().read(3) != 30) return 1;
    }
}
