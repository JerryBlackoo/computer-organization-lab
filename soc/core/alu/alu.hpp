#pragma once

#include "../registers/fr/fr.hpp"

#include <cstdint>

namespace colab::soc::core {

enum class ALUOperation { Add, Subtract };

struct ALUResult {
    std::uint16_t value;
    registers::Flags flags;
};

class ALU {
public:
    ALUResult evaluate(ALUOperation operation, std::uint16_t a, std::uint16_t b) const {
        // 用较宽的无符号数计算，再截取低 16 位，避免宿主 C++ 的有符号溢出。
        const auto wide = operation == ALUOperation::Add
            ? std::uint32_t{a} + b
            : std::uint32_t{a} + 0x10000u - b;
        const auto f = static_cast<std::uint16_t>(wide);
        const bool a15 = (a & 0x8000u) != 0;
        const bool b15 = (b & 0x8000u) != 0;
        const bool f15 = (f & 0x8000u) != 0;

        // SF = F15。
        // 加法 OF = !(A15 xor B15) && (A15 xor F15)。
        // 减法 OF =  (A15 xor B15) && (A15 xor F15)。
        const bool overflow = operation == ALUOperation::Add
            ? (a15 == b15) && (a15 != f15)
            : (a15 != b15) && (a15 != f15);
        // CF is carry-out for addition, unsigned borrow (not carry/no-borrow) for subtraction.
        const bool carry = operation == ALUOperation::Add ? wide > 0xFFFFu : a < b;
        return {f, {f15, overflow, carry, f == 0}};
    }

    ALUResult add(std::uint16_t a, std::uint16_t b) const {
        return evaluate(ALUOperation::Add, a, b);
    }

    ALUResult sub(std::uint16_t a, std::uint16_t b) const {
        return evaluate(ALUOperation::Subtract, a, b);
    }
};
}
