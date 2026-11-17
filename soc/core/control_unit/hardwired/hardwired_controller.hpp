#pragma once

#include "../control_interface.hpp"
#include "../control_logic/control_logic.hpp"
#include "../control_signal_generator/control_signal_generator.hpp"

namespace colab::soc::core::control_unit {

class HardwiredController final : public rv32::ControlInterface {
public:
    void reset() override { timing_ = {}; }
    void abort_instruction() override { timing_.abort_instruction(); }
    rv32::ControlSignals signals(const DecodedInstruction& instruction, rv32::Feedback flags) const override {
        return generator_.generate(phase(), instruction, flags);
    }
    void clock(const DecodedInstruction& instruction, rv32::Feedback feedback) override {
        timing_.on_clock_pulse(logic_.next_phase(phase(), instruction, feedback), instruction_complete());
    }
    rv32::Phase phase() const override { return timing_.phase(); }
    TimingState timing() const override { return timing_.timing(); }
    bool instruction_complete() const override {
        const auto p = phase();
        return p == rv32::Phase::WriteBack || p == rv32::Phase::LoadWriteBack ||
            p == rv32::Phase::StoreWrite || p == rv32::Phase::Branch;
    }
    bool at_instruction_boundary() const override { return phase() == rv32::Phase::FetchAddress; }
    std::uint64_t clock_pulses() const override { return timing_.clock_pulses(); }
    std::uint64_t completed_instructions() const override { return timing_.completed_instructions(); }
private:
    rv32::TimingGenerator timing_;
    ControlLogic logic_;
    ControlSignalGenerator generator_;
};
} // namespace colab::soc::core::control_unit
