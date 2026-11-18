#include "../soc/rv32/core.hpp"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ifstream input(argv[1]);
    std::vector<std::uint32_t> program;
    std::uint32_t word;
    while (input >> std::hex >> word) program.push_back(word);
    colab::soc::RV32Core cpu(program, colab::soc::RV32Core::Mode::Hardwired, 16384);
    for (std::size_t n = 0; n < program.size(); ++n) cpu.set_memory(static_cast<std::uint32_t>(n * 4), program[n]);
    std::cout << std::hex << std::setfill('0');
    try {
        for (unsigned n = 0; n < 2000; ++n) {
            const auto retired = cpu.step_instruction(1024);
            std::cout << "RET " << std::setw(8) << retired.pc << ' ' << std::setw(8) << retired.instruction << ' '
                      << std::dec << bool(retired.register_write) << ' '
                      << unsigned(retired.register_write ? retired.register_write->index : 0) << ' '
                      << std::hex << std::setw(8) << (retired.register_write ? retired.register_write->value : 0) << '\n';
        }
        return 3;
    } catch (const colab::soc::core::RV32Fault& fault) {
        std::cout << "TRAP " << std::setw(8) << fault.pc() << ' ' << std::setw(8)
                  << static_cast<unsigned>(fault.cause()) << ' ' << std::setw(8) << fault.value() << '\n';
    }
    for (unsigned i = 0; i < 32; ++i)
        std::cout << "REG " << std::dec << i << ' ' << std::hex << std::setw(8) << cpu.registers().read(i) << '\n';
    for (std::uint32_t addr : {0x8000u, 0x8004u, 0x8008u})
        std::cout << "MEM " << std::setw(8) << addr << ' ' << std::setw(8) << cpu.memory().read(addr) << '\n';
}
