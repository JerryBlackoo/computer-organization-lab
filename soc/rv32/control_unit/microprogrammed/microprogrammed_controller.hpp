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
        const auto first = store_.at(microprogrammed::ControlStore::fetch_entry);
        sequencer_ = {};
        timing_ = {};
        micro_ir_ = first;
    }
    void abort_instruction() override {
        const auto first = store_.at(microprogrammed::ControlStore::fetch_entry);
        sequencer_ = {};
        micro_ir_ = first;
        timing_.abort_instruction();
    }
    rv32::ControlSignals signals(const DecodedInstruction&, rv32::Feedback) const override {
        return micro_ir_.control; // 没有调用硬布线生成器，也没有按机器操作码执行的 switch。
    }
    void clock(const DecodedInstruction& instruction, rv32::Feedback flags) override {
        const auto next = sequencer_.next(micro_ir_, instruction, flags, store_);
        const auto next_ir = store_.at(next); // 先校验，下址失败不部分推进 μPC/时序。
        timing_.on_clock_pulse(next_ir.phase, instruction_complete());
        sequencer_.latch(next);
        micro_ir_ = next_ir;
    }
    rv32::Phase phase() const override { return micro_ir_.phase; }
    TimingState timing() const override { return timing_.timing(); }
    bool instruction_complete() const override { return micro_ir_.control.retire; }
    bool at_instruction_boundary() const override {
        return sequencer_.address() == microprogrammed::ControlStore::fetch_entry;
    }
    std::uint64_t clock_pulses() const override { return timing_.clock_pulses(); }
    std::uint64_t completed_instructions() const override { return timing_.completed_instructions(); }
    microprogrammed::MicroAddress micro_pc() const { return sequencer_.address(); }
    const microprogrammed::Microinstruction& micro_ir() const { return micro_ir_; }
    const microprogrammed::ControlStore& control_store() const { return store_; }
private:
    microprogrammed::ControlStore store_;
    microprogrammed::Microsequencer sequencer_;
    microprogrammed::Microinstruction micro_ir_;
    rv32::TimingGenerator timing_;
};
} // namespace colab::soc::core::control_unit
