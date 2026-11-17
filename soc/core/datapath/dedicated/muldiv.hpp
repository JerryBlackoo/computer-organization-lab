#pragma once

#include "alu_arithmetic.hpp"

#include <cstdint>
#include <stdexcept>

namespace colab::soc::core::datapath::dedicated {

// Clocked 32-bit P:Q multiplier / R:Q divider. start() latches operands;
// Each step() uses the 32-bit ALU at most once and advances one bit.
// Optional signed-result correction uses additional ALU beats.
class MulDiv {
public:
    struct Division { std::uint32_t quotient, remainder; };
    enum class Action { Start, NoAdd, Add, Subtract, Restore };
    static constexpr unsigned iterations = 32;

    void reset() { *this = MulDiv{}; }
    bool busy() const { return busy_; }
    bool done() const { return done_; }
    unsigned iteration() const { return iteration_; }
    unsigned beat() const { return beat_; }
    ALUCtr control() const { return control_; }
    std::uint32_t operand_a() const { return a_; }
    std::uint32_t operand_b() const { return b_; }
    // P for multiplication or R for division; the 33rd bit is preserved.
    std::uint64_t partial() const { return partial_; }
    std::uint32_t q() const { return q_; }
    std::uint32_t divisor() const { return divisor_; }
    bool previous_bit() const { return previous_; }
    bool carry() const { return carry_; }
    Action last_action() const { return action_; }
    bool alu_used() const { return alu_used_; }
    ALUCtr alu_control() const { return alu_control_; }
    std::uint32_t alu_a() const { return alu_a_; }
    std::uint32_t alu_b() const { return alu_b_; }
    std::uint32_t alu_result() const { return alu_result_; }
    bool alu_carry_or_borrow() const { return alu_carry_; }

    void start(ALUCtr control, std::uint32_t a, std::uint32_t b) {
        if (busy_) throw std::logic_error("multiply/divide unit is busy");
        if (!is_multiply(control) && !is_divide(control))
            throw std::invalid_argument("not a multiply/divide control");
        reset();
        control_ = control; a_ = a; b_ = b;
        if (is_divide(control)) {
            // RISC-V special results do not trap and can complete on the start beat.
            const bool signed_op = control == ALUCtr::Div || control == ALUCtr::Rem;
            if (!b) {
                q_ = UINT32_MAX; partial_ = a;
                if (signed_op && (a >> 31)) partial_ |= std::uint64_t{1} << 32;
                division_ = {q_, a}; done_ = true; return;
            }
            if (signed_op && a == 0x80000000u && b == 0xffffffffu) {
                q_ = a; division_ = {a, 0}; done_ = true; return;
            }
            const bool negative_a = signed_op && (a >> 31);
            const bool negative_b = signed_op && (b >> 31);
            negative_quotient_ = negative_a != negative_b;
            negative_remainder_ = negative_a;
            // Unsigned negation is defined even for the magnitude of INT_MIN.
            q_ = negative_a ? 0u - a : a;
            divisor_ = negative_b ? 0u - b : b;
        } else q_ = b;
        busy_ = true;
    }

    void step() { step(ALUArithmetic{}); }

    template<class Arithmetic>
    void step(const Arithmetic& alu) {
        if (!busy_) throw std::logic_error("multiply/divide unit is not busy");
        alu_used_ = false;
        if (post_ != Post::None) {
            ++beat_;
            if (post_ == Post::MixedHigh) {
                const auto high = run_alu(alu, ALUCtr::Addu, static_cast<std::uint32_t>(product_ >> 32), a_).result;
                product_ = (std::uint64_t{high} << 32) | q_;
                partial_ = std::uint64_t{high} | ((high >> 31) ? (std::uint64_t{1} << 32) : 0);
                action_ = Action::Add;
                post_ = Post::None;
            } else if (post_ == Post::Quotient) {
                division_.quotient = run_alu(alu, ALUCtr::Subu, 0, division_.quotient).result;
                q_ = division_.quotient; action_ = Action::Subtract;
                post_ = negative_remainder_ && division_.remainder ? Post::Remainder : Post::None;
            } else {
                division_.remainder = run_alu(alu, ALUCtr::Subu, 0, division_.remainder).result;
                partial_ = std::uint64_t{division_.remainder} | (std::uint64_t{1} << 32);
                action_ = Action::Subtract; post_ = Post::None;
            }
            if (post_ == Post::None) { busy_ = false; done_ = true; }
            return;
        }
        if (control_ == ALUCtr::Mul || control_ == ALUCtr::Mulhu) {
            // The ALU and shifter are combinational in this beat; C:P:Q
            // is latched once at the next edge.
            action_ = (q_ & 1u) ? Action::Add : Action::NoAdd;
            auto low = static_cast<std::uint32_t>(partial_);
            carry_ = false;
            if (q_ & 1u) {
                const auto sum = run_alu(alu, ALUCtr::Addu, low, a_);
                low = sum.result; carry_ = sum.carry;
            }
            q_ = (q_ >> 1) | (low << 31);
            partial_ = (low >> 1) | (std::uint32_t{carry_} << 31);
        } else if (control_ == ALUCtr::Mulh || control_ == ALUCtr::Mulhsu) {
            // 33-bit Booth accumulator: upper bit is computed from the
            // 32-bit ALU's carry/borrow and the two sign-extension bits.
            constexpr std::uint64_t sign = std::uint64_t{1} << 32;
            const bool current = (q_ & 1u) != 0;
            action_ = Action::NoAdd;
            if (current != previous_) {
                const auto op = current ? ALUCtr::Subu : ALUCtr::Addu;
                const auto value = run_alu(alu, op, static_cast<std::uint32_t>(partial_), a_);
                const auto high = ((partial_ >> 32) ^ (a_ >> 31) ^ unsigned(value.carry)) & 1u;
                partial_ = (high << 32) | value.result;
                action_ = current ? Action::Subtract : Action::Add;
            }
            previous_ = current;
            q_ = (q_ >> 1) | (static_cast<std::uint32_t>(partial_) << 31);
            partial_ = (partial_ >> 1) | (partial_ & sign);
        } else {
            // The high remainder bit plus ALU borrow decides the 33-bit trial.
            partial_ = (partial_ << 1) | (q_ >> 31);
            q_ <<= 1;
            const auto trial = run_alu(alu, ALUCtr::Subu, static_cast<std::uint32_t>(partial_), divisor_);
            const auto high = partial_ >> 32;
            const bool negative = !high && trial.carry;
            action_ = negative ? Action::Restore : Action::Subtract;
            if (!negative) {
                partial_ = ((high - unsigned(trial.carry)) << 32) | trial.result;
                q_ |= 1u;
            }
        }
        ++beat_;
        if (++iteration_ != iterations) return;
        if (is_multiply(control_)) {
            product_ = (std::uint64_t{static_cast<std::uint32_t>(partial_)} << 32) | q_;
            if (control_ == ALUCtr::Mulhsu && (b_ >> 31)) {
                post_ = Post::MixedHigh; return;
            }
        } else {
            division_ = {q_, static_cast<std::uint32_t>(partial_)};
            if (negative_quotient_ && division_.quotient) { post_ = Post::Quotient; return; }
            if (negative_remainder_ && division_.remainder) { post_ = Post::Remainder; return; }
        }
        busy_ = false; done_ = true;
    }

    std::uint32_t result() const {
        if (!done_) throw std::logic_error("multiply/divide result is not ready");
        switch (control_) {
        case ALUCtr::Mul: return static_cast<std::uint32_t>(product_);
        case ALUCtr::Mulh: case ALUCtr::Mulhsu: case ALUCtr::Mulhu:
            return static_cast<std::uint32_t>(product_ >> 32);
        case ALUCtr::Div: case ALUCtr::Divu: return division_.quotient;
        case ALUCtr::Rem: case ALUCtr::Remu: return division_.remainder;
        default: throw std::logic_error("invalid multiply/divide result control");
        }
    }
    std::uint64_t product() const {
        if (!done_ || !is_multiply(control_)) throw std::logic_error("product is not ready");
        return product_;
    }
    Division division() const {
        if (!done_ || !is_divide(control_)) throw std::logic_error("division is not ready");
        return division_;
    }

    // Combinational convenience interface for the standalone ALU. It runs
    // the very same clocked steps; CPU execution does not call these helpers.
    static std::uint64_t multiply_unsigned(std::uint32_t x, std::uint32_t y) {
        return calculate(ALUCtr::Mulhu, x, y).product();
    }
    static std::uint64_t multiply_signed(std::uint32_t x, std::uint32_t y) {
        return calculate(ALUCtr::Mulh, x, y).product();
    }
    static std::uint64_t multiply_signed_unsigned(std::uint32_t x, std::uint32_t y) {
        return calculate(ALUCtr::Mulhsu, x, y).product();
    }
    static Division divide_unsigned(std::uint32_t dividend, std::uint32_t divisor) {
        return calculate(ALUCtr::Divu, dividend, divisor).division();
    }
    static Division divide_signed(std::uint32_t dividend, std::uint32_t divisor) {
        return calculate(ALUCtr::Div, dividend, divisor).division();
    }

private:
    enum class Post { None, MixedHigh, Quotient, Remainder };
    template<class Arithmetic>
    ALUResult run_alu(const Arithmetic& alu, ALUCtr op, std::uint32_t a, std::uint32_t b) {
        const auto value = alu.evaluate(op, a, b);
        alu_used_ = true; alu_control_ = op; alu_a_ = a; alu_b_ = b; alu_result_ = value.result;
        alu_carry_ = value.carry;
        return value;
    }
    static bool is_multiply(ALUCtr op) {
        return op == ALUCtr::Mul || op == ALUCtr::Mulh || op == ALUCtr::Mulhsu || op == ALUCtr::Mulhu;
    }
    static bool is_divide(ALUCtr op) {
        return op == ALUCtr::Div || op == ALUCtr::Divu || op == ALUCtr::Rem || op == ALUCtr::Remu;
    }
    static MulDiv calculate(ALUCtr op, std::uint32_t a, std::uint32_t b) {
        MulDiv unit; unit.start(op, a, b);
        while (unit.busy()) unit.step();
        return unit;
    }

    ALUCtr control_ = ALUCtr::Mul;
    std::uint32_t a_ = 0, b_ = 0, q_ = 0, divisor_ = 0;
    std::uint64_t partial_ = 0, product_ = 0;
    Division division_{0, 0};
    Action action_ = Action::Start;
    Post post_ = Post::None;
    ALUCtr alu_control_ = ALUCtr::Addu;
    std::uint32_t alu_a_ = 0, alu_b_ = 0, alu_result_ = 0;
    unsigned iteration_ = 0, beat_ = 0;
    bool busy_ = false, done_ = false, previous_ = false, alu_used_ = false;
    bool carry_ = false, alu_carry_ = false;
    bool negative_quotient_ = false, negative_remainder_ = false;
};
} // namespace colab::soc::core::datapath::dedicated
