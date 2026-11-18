#pragma once

#include "microsequencer.hpp"
#include <utility>

namespace colab::soc::core::control_unit {

class MicroprogrammedController final : public rv32::ControlInterface {
public:
    explicit MicroprogrammedController(microprogrammed::ControlStore store = {}) : store_(std::move(store)) {
        reset();
    }
    void reset() override {
        const auto first = store_.raw_at(microprogrammed::ControlStore::fetch_entry);
        sequencer_ = {};
        timing_ = {};
        micro_ir_ = first;
        phase_ = store_.phase_at(microprogrammed::ControlStore::fetch_entry);
    }
    void abort_instruction() override {
        const auto first = store_.raw_at(microprogrammed::ControlStore::fetch_entry);
        sequencer_ = {};
        micro_ir_ = first;
        phase_ = store_.phase_at(microprogrammed::ControlStore::fetch_entry);
        timing_.abort_instruction();
    }
    rv32::ControlSignals signals(const DecodedInstruction&, rv32::Feedback) const override {
        return microprogrammed::decode(micro_ir_, phase_).control;
    }
    void clock(const DecodedInstruction& instruction, rv32::Feedback flags) override {
        const auto next = sequencer_.next(microprogrammed::decode(micro_ir_, phase_), instruction, flags, store_);
        const auto next_ir = store_.raw_at(next); // Validate before advancing any state.
        const auto next_phase = store_.phase_at(next);
        timing_.on_clock_pulse(next_phase, instruction_complete());
        sequencer_.latch(next);
        micro_ir_ = next_ir;
        phase_ = next_phase;
    }
    rv32::Phase phase() const override { return phase_; }
    TimingState timing() const override { return timing_.timing(); }
    bool instruction_complete() const override { return (micro_ir_ >> 16) & 1u; }
    bool at_instruction_boundary() const override {
        return sequencer_.address() == microprogrammed::ControlStore::fetch_entry;
    }
    std::uint64_t clock_pulses() const override { return timing_.clock_pulses(); }
    std::uint64_t completed_instructions() const override { return timing_.completed_instructions(); }
    microprogrammed::MicroAddress micro_pc() const { return sequencer_.address(); }
    microprogrammed::Microinstruction micro_ir() const { return microprogrammed::decode(micro_ir_, phase_); }
    microprogrammed::MicroWord micro_ir_word() const { return micro_ir_; }
    const microprogrammed::ControlStore& control_store() const { return store_; }
private:
    microprogrammed::ControlStore store_;
    microprogrammed::Microsequencer sequencer_;
    microprogrammed::MicroWord micro_ir_ = 0;
    rv32::Phase phase_ = rv32::Phase::FetchAddress;
    rv32::TimingGenerator timing_;
};
} // namespace colab::soc::core::control_unit
