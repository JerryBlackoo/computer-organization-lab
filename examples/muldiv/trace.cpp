#include "../../soc/core/rv32_core.hpp"

#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using CPU = colab::soc::RV32Core;
using MulDiv = colab::soc::core::datapath::dedicated::MulDiv;

static std::uint32_t parse_word(const std::string& text) {
    std::size_t end = 0;
    const auto value = std::stoll(text, &end, 0);
    if (end != text.size() || value < -0x80000000ll || value > 0xffffffffll)
        throw std::invalid_argument("operand must fit a 32-bit signed or unsigned word");
    return static_cast<std::uint32_t>(value);
}
static const char* action(MulDiv::Action value) {
    switch (value) {
    case MulDiv::Action::Start: return "start";
    case MulDiv::Action::NoAdd: return "no add";
    case MulDiv::Action::Add: return "add";
    case MulDiv::Action::Subtract: return "subtract";
    case MulDiv::Action::Restore: return "restore";
    }
    throw std::logic_error("unknown M action");
}
int main(int argc, char** argv) {
    try {
        if (argc > 5) throw std::invalid_argument("usage: trace [mul|mulh|mulhsu|mulhu|div|divu|rem|remu] [a] [b] [hardwired|micro|pipeline]");
        const std::array<std::string, 8> names{"mul", "mulh", "mulhsu", "mulhu", "div", "divu", "rem", "remu"};
        const std::string operation = argc > 1 ? argv[1] : "mul";
        unsigned funct3 = 0;
        while (funct3 < names.size() && names[funct3] != operation) ++funct3;
        if (funct3 == names.size()) throw std::invalid_argument("unknown operation");
        const auto a = argc > 2 ? parse_word(argv[2]) : 3u;
        const auto b = argc > 3 ? parse_word(argv[3]) : 5u;
        const std::string mode = argc > 4 ? argv[4] : "hardwired";
        if (mode != "hardwired" && mode != "micro" && mode != "pipeline")
            throw std::invalid_argument("unknown CPU mode");
        const auto instruction = (1u << 25) | (2u << 20) | (1u << 15) | (funct3 << 12) | (3u << 7) | 0x33u;
        CPU cpu({instruction}, mode == "micro" ? CPU::Mode::Microprogrammed : CPU::Mode::Hardwired, 16, {},
                mode == "pipeline" ? CPU::Execution::Pipelined : CPU::Execution::MultiCycle);
        cpu.set_register(1, a); cpu.set_register(2, b);
        std::cout << "cycle beat bit action    P/R (33 bits)  Q         Q-1 C busy done\n";
        while (!cpu.completed_instructions()) {
            cpu.tick();
            const auto& trace = *cpu.last_cycle();
            if (!trace.muldiv) continue;
            const auto& unit = *trace.muldiv;
            std::cout << std::dec << std::setw(5) << cpu.clock_pulses() << ' '
                      << std::setw(4) << unit.beat() << ' ' << std::setw(3) << unit.iteration() << ' '
                      << std::left << std::setw(9) << action(unit.last_action()) << std::right << " 0x"
                      << std::hex << std::setfill('0') << std::setw(9) << unit.partial() << " 0x"
                      << std::setw(8) << unit.q() << std::setfill(' ') << std::dec
                      << "  " << unit.previous_bit() << "  " << unit.carry() << "   " << unit.busy()
                      << "    " << unit.done();
            if (unit.alu_used()) {
                std::cout << "  ALU " << (unit.alu_control() == colab::soc::core::datapath::dedicated::ALUCtr::Addu ? '+' : '-')
                          << " 0x" << std::hex << std::setfill('0') << std::setw(8) << unit.alu_a()
                          << " 0x" << std::setw(8) << unit.alu_b() << " = 0x" << std::setw(8) << unit.alu_result()
                          << std::setfill(' ') << std::dec << " carry/borrow=" << unit.alu_carry_or_borrow();
            }
            std::cout << '\n';
        }
        std::cout << "retired after " << std::dec << cpu.clock_pulses() << " cycles; x3=0x"
                  << std::hex << std::setfill('0') << std::setw(8) << cpu.registers().read(3) << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
