#pragma once

#include "alu_arithmetic.hpp"
#include "muldiv.hpp"

#include <cstdint>
#include <stdexcept>

namespace colab::soc::core::datapath::dedicated {

// 与 16 位单总线 ALU 独立，专用通路宽度严格为 32 位。
class ALU {
public:
    ALUResult evaluate(ALUCtr control, std::uint32_t a, std::uint32_t b) const {
        std::uint32_t result = 0;
        switch (control) {
        case ALUCtr::Mul: result = static_cast<std::uint32_t>(MulDiv::multiply_unsigned(a, b)); break;
        case ALUCtr::Mulh: result = static_cast<std::uint32_t>(MulDiv::multiply_signed(a, b) >> 32); break;
        case ALUCtr::Mulhsu: result = static_cast<std::uint32_t>(MulDiv::multiply_signed_unsigned(a, b) >> 32); break;
        case ALUCtr::Mulhu: result = static_cast<std::uint32_t>(MulDiv::multiply_unsigned(a, b) >> 32); break;
        case ALUCtr::Div: result = MulDiv::divide_signed(a, b).quotient; break;
        case ALUCtr::Divu: result = MulDiv::divide_unsigned(a, b).quotient; break;
        case ALUCtr::Rem: result = MulDiv::divide_signed(a, b).remainder; break;
        case ALUCtr::Remu: result = MulDiv::divide_unsigned(a, b).remainder; break;
        default: return arithmetic_.evaluate(control, a, b);
        }
        return {result, result == 0, false};
    }
private:
    ALUArithmetic arithmetic_;
};
}
