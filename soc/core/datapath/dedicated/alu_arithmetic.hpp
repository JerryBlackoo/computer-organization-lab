#pragma once

#include "alu_control.hpp"
#include <cstdint>
#include <stdexcept>

namespace colab::soc::core::datapath::dedicated {

struct ALUResult {
    std::uint32_t result;
    bool zero;
    bool overflow;
    // For addition this is carry-out; for subtraction it is borrow-out.
    bool carry = false;
};

// Shared 32-bit combinational ALU used by ordinary EX and the iterative MDU.
class ALUArithmetic {
public:
    ALUResult evaluate(ALUCtr control, std::uint32_t a, std::uint32_t b) const {
        std::uint32_t result = 0;
        bool overflow = false, carry = false;
        switch (control) {
        case ALUCtr::Add: case ALUCtr::Addu:
            result = a + b;
            carry = result < a;
            overflow = control == ALUCtr::Add && ((~(a ^ b) & (a ^ result)) >> 31);
            break;
        case ALUCtr::Sub: case ALUCtr::Subu:
            result = a - b;
            carry = a < b;
            overflow = control == ALUCtr::Sub && (((a ^ b) & (a ^ result)) >> 31);
            break;
        case ALUCtr::And: result = a & b; break;
        case ALUCtr::Or: result = a | b; break;
        case ALUCtr::Xor: result = a ^ b; break;
        case ALUCtr::Nor: result = ~(a | b); break;
        case ALUCtr::Slt: result = (a ^ 0x80000000u) < (b ^ 0x80000000u); break;
        case ALUCtr::Sltu: result = a < b; break;
        case ALUCtr::Sll: result = a << (b & 31u); break;
        case ALUCtr::Srl: result = a >> (b & 31u); break;
        case ALUCtr::Sra: {
            const auto shift = b & 31u;
            result = a >> shift;
            if (shift && (a & 0x80000000u)) result |= UINT32_MAX << (32u - shift);
            break;
        }
        default: throw std::invalid_argument("unknown arithmetic ALU control");
        }
        return {result, result == 0, overflow, carry};
    }
};
} // namespace colab::soc::core::datapath::dedicated
