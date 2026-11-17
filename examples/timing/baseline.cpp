#include "../../soc/core/rv32_core.hpp"
#include <array>
#include <iomanip>
#include <iostream>

using CPU = colab::soc::RV32Core;
struct Example { const char* name; std::uint32_t instruction, a, b; };

int main() {
    // Each sample starts from an empty core with local zero-wait memory.
    const std::array<Example, 9> examples{{
        {"ADD", 0x002081b3u, 13, 3}, {"LW", 0x00002183u, 0, 0},
        {"SW", 0x00202023u, 0, 3}, {"BEQ", 0x00208463u, 3, 3},
        {"MUL", 0x022081b3u, 13, 3}, {"DIVU", 0x0220d1b3u, 13, 3},
        {"DIV -13/3", 0x0220c1b3u, 0xfffffff3u, 3},
        {"DIV by 0", 0x0220c1b3u, 13, 0}, {"DIV overflow", 0x0220c1b3u, 0x80000000u, 0xffffffffu}}};
    std::cout << "instruction        hardwired  microprogram  pipeline\n";
    for (const auto& sample : examples) {
        std::cout << std::left << std::setw(19) << sample.name;
        for (unsigned mode = 0; mode < 3; ++mode) {
            CPU cpu({sample.instruction}, mode == 1 ? CPU::Mode::Microprogrammed : CPU::Mode::Hardwired,
                    16, {}, mode == 2 ? CPU::Execution::Pipelined : CPU::Execution::MultiCycle);
            cpu.set_register(1, sample.a); cpu.set_register(2, sample.b);
            std::cout << std::right << std::setw(mode == 1 ? 13 : 10) << cpu.step_instruction(256).cycles;
        }
        std::cout << '\n';
    }
}
