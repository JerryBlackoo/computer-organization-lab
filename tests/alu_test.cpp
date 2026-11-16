#include "../soc/core/alu/alu.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <type_traits>

// 用数学范围判定作独立参照，不复写 ALU 中基于符号位的 OF 公式。
std::int32_t signed_value(std::uint16_t bits) {
    return bits < 0x8000u ? bits : static_cast<std::int32_t>(bits) - 65536;
}

int main() {
    using colab::soc::core::registers::Flags;
    static_assert(std::is_aggregate<Flags>::value, "Flags positional initialization must remain compatible");
    constexpr Flags historical{true, false};
    static_assert(historical.sf && !historical.of && !historical.cf && !historical.zf,
                  "appended fields must not reinterpret old SF/OF initializers");
    const colab::soc::core::ALU alu;
    const std::uint16_t partners[] = {0, 1, 2, 0x7FFE, 0x7FFF, 0x8000, 0x8001, 0xFFFE, 0xFFFF};
    for (std::uint32_t a = 0; a <= 0xFFFFu; ++a) {
        const auto bits_a = static_cast<std::uint16_t>(a);
        for (const auto b : partners) {
            const auto sum = signed_value(bits_a) + signed_value(b);
            const auto difference = signed_value(bits_a) - signed_value(b);
            const auto add = alu.add(bits_a, b);
            const auto sub = alu.sub(bits_a, b);
            assert(add.value == static_cast<std::uint16_t>(sum));
            assert(sub.value == static_cast<std::uint16_t>(difference));
            assert(add.flags.sf == (signed_value(add.value) < 0));
            assert(sub.flags.sf == (signed_value(sub.value) < 0));
            assert(add.flags.of == (sum < -32768 || sum > 32767));
            assert(sub.flags.of == (difference < -32768 || difference > 32767));
            assert(add.flags.cf == (a + b >= 65536u));
            assert(sub.flags.cf == (static_cast<std::int32_t>(a) - b < 0));
            assert(add.flags.zf == (sum % 65536 == 0));
            assert(sub.flags.zf == (difference % 65536 == 0));
        }
    }

    // SF 与 OF 的四种组合都能出现。
    assert(!alu.add(1, 1).flags.sf && !alu.add(1, 1).flags.of);
    assert(alu.add(0xFFFF, 0).flags.sf && !alu.add(0xFFFF, 0).flags.of);
    assert(alu.add(0x7FFF, 1).flags.sf && alu.add(0x7FFF, 1).flags.of);
    assert(!alu.add(0x8000, 0xFFFF).flags.sf && alu.add(0x8000, 0xFFFF).flags.of);
    // -32768 - (-32768) = 0，减法 OF 必须使用原始 B 的符号。
    assert(alu.sub(0x8000, 0x8000).value == 0);
    assert(!alu.sub(0x8000, 0x8000).flags.of);
    // Carry without signed overflow; signed overflow without carry; borrow; zero.
    const auto carry = alu.add(0xFFFF, 1), overflow = alu.add(0x7FFF, 1), borrow = alu.sub(0, 1);
    assert(carry.value == 0 && carry.flags.cf && carry.flags.zf && !carry.flags.of && !carry.flags.sf);
    assert(overflow.flags.of && overflow.flags.sf && !overflow.flags.cf && !overflow.flags.zf);
    assert(borrow.value == 0xFFFF && borrow.flags.cf && borrow.flags.sf && !borrow.flags.of && !borrow.flags.zf);
    for (std::uint32_t a = 0; a <= 0xFFFFu; ++a) {
        const auto self = alu.sub(static_cast<std::uint16_t>(a), static_cast<std::uint16_t>(a));
        assert(self.value == 0 && self.flags.zf && !self.flags.cf && !self.flags.of && !self.flags.sf);
    }
    std::cout << "PASS 16-bit ALU: 1179648 boundary-pair add/sub + 65536 self-sub cases, SF/OF/CF/ZF and aggregate compatibility\n";
}
