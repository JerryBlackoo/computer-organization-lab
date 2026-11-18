#pragma once

#include "../soc/rv32/core.hpp"
#include <cassert>

namespace rv32_test {
using CPU = colab::soc::RV32Core;
using Mode = CPU::Mode;
using Operation = colab::soc::core::Operation;
using Fault = colab::soc::core::RV32Fault;
using Cause = colab::soc::core::RV32FaultCause;
using Phase = colab::soc::core::control_unit::rv32::Phase;

inline std::uint32_t r(unsigned rd, unsigned rs1, unsigned rs2, bool sub = false) {
    assert(rd < 32 && rs1 < 32 && rs2 < 32);
    return (sub ? 0x40000000u : 0u) | (rs2 << 20) | (rs1 << 15) | (rd << 7) | 0x33u;
}
inline std::uint32_t i(unsigned rd, unsigned rs1, int immediate, bool load = false) {
    assert(rd < 32 && rs1 < 32 && immediate >= -2048 && immediate <= 2047);
    return ((static_cast<std::uint32_t>(immediate) & 0xFFFu) << 20) | (rs1 << 15)
        | (load ? 0x2000u : 0u) | (rd << 7) | (load ? 0x03u : 0x13u);
}
inline std::uint32_t sw(unsigned rs2, unsigned rs1, int immediate) {
    assert(rs1 < 32 && rs2 < 32 && immediate >= -2048 && immediate <= 2047);
    const auto bits = static_cast<std::uint32_t>(immediate) & 0xFFFu;
    return ((bits >> 5) << 25) | (rs2 << 20) | (rs1 << 15) | 0x2000u | ((bits & 31u) << 7) | 0x23u;
}
inline std::uint32_t beq(unsigned rs1, unsigned rs2, int immediate) {
    assert(rs1 < 32 && rs2 < 32 && immediate >= -4096 && immediate <= 4094 && immediate % 2 == 0);
    const auto bits = static_cast<std::uint32_t>(immediate) & 0x1FFFu;
    return ((bits >> 12) << 31) | (((bits >> 5) & 63u) << 25) | (rs2 << 20)
        | (rs1 << 15) | (((bits >> 1) & 15u) << 8) | (((bits >> 11) & 1u) << 7) | 0x63u;
}

template<class Exception, class F> void expect_throw(F function) {
    bool threw = false;
    try { function(); }
    catch (const Exception&) { threw = true; }
    assert(threw);
}
inline void compare_architecture(const CPU& a, const CPU& b) {
    assert(a.pc() == b.pc());
    assert(a.completed_instructions() == b.completed_instructions());
    for (unsigned index = 0; index < 32; ++index) assert(a.registers().read(index) == b.registers().read(index));
    assert(a.data_words() == b.data_words());
    for (std::size_t index = 0; index < a.data_words(); ++index)
        assert(a.memory().read(static_cast<std::uint32_t>(index * 4)) ==
               b.memory().read(static_cast<std::uint32_t>(index * 4)));
}
} // namespace rv32_test
