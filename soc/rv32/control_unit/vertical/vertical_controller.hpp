#pragma once

#include "../microprogrammed/microsequencer.hpp"
#include <array>
#include <bitset>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace colab::soc::core::control_unit::vertical {
// 48-bit word: [4:0] micro-operation; [21:5] operands/selectors;
// [40:22] sequence and two 8-bit addresses; [47:41] reserved.
// Each opcode names one supported datapath action. Parameter fields select
// ALU function, width and other mutually exclusive choices.
using Word = std::uint64_t;
inline constexpr unsigned word_bits = 48;
enum class Op : unsigned {
    Nop, FetchAddress, FetchRead, FetchCapture, LatchIR, Decode,
    ReadOperands, MulStart, MulStep, Alu, EffectiveAddress, Compare,
    LoadRead, LoadCapture, Finish, Store, WriteRegister, System
};
inline constexpr std::array<std::uint32_t, 18> op_masks{{
    0x00000, 0x00001, 0x00002, 0x00006, 0x00008, 0x00010,
    0x00020, 0x00040, 0x00080, 0x00100, 0x00300, 0x00400,
    0x00800, 0x01800, 0x18000, 0x1a000, 0x1c000, 0x38000
}};
inline Word assemble(microprogrammed::MicroWord horizontal) {
    (void)microprogrammed::decode(horizontal);
    const auto mask = static_cast<std::uint32_t>(horizontal & 0x3ffffu);
    unsigned opcode = 0;
    while (opcode < op_masks.size() && op_masks[opcode] != mask) ++opcode;
    if (opcode == op_masks.size()) throw std::invalid_argument("control combination has no vertical micro-op");
    return Word{opcode} | (((horizontal >> 18) & 0x1ffffu) << 5)
        | (((horizontal >> 35) & 0x7ffffu) << 22);
}
inline microprogrammed::Microinstruction decode(Word word, rv32::Phase phase) {
    const unsigned opcode = static_cast<unsigned>(word & 31u);
    if ((word >> 41) || opcode >= op_masks.size())
        throw std::invalid_argument("invalid vertical microinstruction");
    const auto horizontal = microprogrammed::MicroWord{op_masks[opcode]}
        | (((word >> 5) & 0x1ffffu) << 18) | (((word >> 22) & 0x7ffffu) << 35);
    return microprogrammed::decode(horizontal, phase);
}

class Controller final : public rv32::ControlInterface {
public:
    explicit Controller(microprogrammed::ControlStore source = {}) : source_(std::move(source)) {
        for (microprogrammed::MicroAddress i = 0; i < source_.capacity; ++i) {
            if (source_.valid(i)) { words_[i] = assemble(source_.raw_at(i)); valid_.set(i); }
        }
        reset();
    }
    void reset() override { sequencer_ = {}; timing_ = {}; load(0); }
    void abort_instruction() override { sequencer_ = {}; load(0); timing_.abort_instruction(); }
    rv32::ControlSignals signals(const DecodedInstruction&, rv32::Feedback) const override {
        return decode(word_, phase_).control;
    }
    void clock(const DecodedInstruction& instruction, rv32::Feedback flags) override {
        auto next = sequencer_.next(decode(word_, phase_), instruction, flags, source_);
        const auto next_word = at(next);
        const auto next_phase = source_.phase_at(next);
        timing_.on_clock_pulse(next_phase, instruction_complete());
        sequencer_.latch(next); word_ = next_word; phase_ = next_phase;
    }
    rv32::Phase phase() const override { return phase_; }
    TimingState timing() const override { return timing_.timing(); }
    bool instruction_complete() const override { return decode(word_, phase_).control.retire; }
    bool at_instruction_boundary() const override { return sequencer_.address() == 0; }
    std::uint64_t clock_pulses() const override { return timing_.clock_pulses(); }
    std::uint64_t completed_instructions() const override { return timing_.completed_instructions(); }
    microprogrammed::MicroAddress micro_pc() const { return sequencer_.address(); }
    Word micro_ir_word() const { return word_; }
    Word at(microprogrammed::MicroAddress address) const {
        const auto w = words_.at(address);
        if (!valid_.test(address)) throw std::logic_error("uninitialized vertical control-store address");
        return w;
    }
private:
    void load(microprogrammed::MicroAddress address) { word_ = at(address); phase_ = source_.phase_at(address); }
    microprogrammed::ControlStore source_; // entry mapping and phase labels
    std::array<Word, 256> words_{};
    std::bitset<256> valid_{};
    microprogrammed::Microsequencer sequencer_;
    rv32::TimingGenerator timing_;
    Word word_ = 0;
    rv32::Phase phase_ = rv32::Phase::FetchAddress;
};
} // namespace colab::soc::core::control_unit::vertical
