#include "../../soc/rv32/core.hpp"
#include <bitset>
#include <iomanip>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    using CPU = colab::soc::RV32Core;
    const std::string choice = argc > 1 ? argv[1] : "horizontal";
    if (choice != "horizontal" && choice != "vertical") {
        std::cerr << "usage: binary_trace [horizontal|vertical]\n"; return 2;
    }
    const bool vertical = choice == "vertical";
    CPU cpu({0x002081b3}, vertical ? CPU::Mode::VerticalMicroprogrammed : CPU::Mode::Microprogrammed);
    cpu.set_register(1, 10); cpu.set_register(2, 20);
    std::cout << choice << " ADD x3,x1,x2: x1=10 x2=20\n";
    while (cpu.completed_instructions() == 0) {
        const auto& unit = cpu.control_unit();
        const auto signal = unit.signals({}, {});
        std::cout << "uPC=" << std::setw(3) << *unit.micro_address() << " uIR=";
        if (vertical) {
            const auto word = unit.vertical().micro_ir_word();
            std::cout << std::bitset<48>(word) << " uOP=" << (word & 31u);
        } else std::cout << std::bitset<64>(unit.microprogrammed().micro_ir_word());
        std::cout << " operands_in=" << signal.operands_in << " alu_in=" << signal.alu_in
                  << " reg_write=" << signal.register_write << '\n';
        cpu.tick();
    }
    std::cout << "x3=" << cpu.registers().read(3) << "\n";
}
