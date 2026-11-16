#pragma once
#include "timing_generator/timing_generator.hpp"
#include "control_logic/control_logic.hpp"
#include "control_signal_generator/control_signal_generator.hpp"
#include "hardwired/hardwired_controller.hpp"
#include "microprogrammed/microprogrammed_controller.hpp"

#include <utility>

namespace colab::soc::core {
class ControlUnit {
public:
    using Mode = control_unit::ControlMode;
    explicit ControlUnit(Mode mode = Mode::Hardwired,
                         control_unit::microprogrammed::ControlStore store = {})
        : mode_(mode), microprogrammed_(std::move(store)) {
        validate_mode(mode);
    }

    // RV32 接口。任意一次调用只访问当前模式的一个控制器。
    Mode mode() const { return mode_; }
    void select_mode(Mode mode) {
        validate_mode(mode);
        if (!active().at_instruction_boundary())
            throw std::logic_error("control mode can change only at an instruction boundary");
        mode_ = mode;
    }
    void reset() {
        timing_ = {};
        hardwired_.reset(); microprogrammed_.reset();
        rv32_pulses_ = 0; rv32_retired_ = 0;
    }
    control_unit::rv32::ControlSignals signals(const DecodedInstruction& instruction,
                                              control_unit::rv32::Feedback flags) const {
        return active().signals(instruction, flags);
    }
    void clock(const DecodedInstruction& instruction, control_unit::rv32::Feedback flags) {
        const bool retire = active().instruction_complete();
        active().clock(instruction, flags);
        ++rv32_pulses_;
        if (retire) ++rv32_retired_;
    }
    // Ready/valid wait: count real clocks, hold control phase and architectural state.
    void wait_clock() { ++rv32_pulses_; }
    void abort_instruction() { active().abort_instruction(); ++rv32_pulses_; }
    control_unit::rv32::Phase rv32_phase() const { return active().phase(); }
    control_unit::TimingState rv32_timing() const {
        auto state = active().timing();
        state.clock_pulse = rv32_pulses_ + 1;
        state.instruction_cycle = rv32_retired_ + 1;
        return state;
    }
    bool rv32_instruction_complete() const { return active().instruction_complete(); }
    bool at_instruction_boundary() const { return active().at_instruction_boundary(); }
    std::uint64_t rv32_clock_pulses() const { return rv32_pulses_; }
    std::uint64_t rv32_completed_instructions() const { return rv32_retired_; }
    const control_unit::HardwiredController& hardwired() const { return hardwired_; }
    const control_unit::MicroprogrammedController& microprogrammed() const { return microprogrammed_; }

    // 原有 16 位教学 Core 的接口保持不变。
    control_unit::Phase phase() const { return timing_.phase(); }
    control_unit::InstructionStage stage() const { return timing_.stage(); }
    control_unit::TimingState timing() const { return timing_.timing(); }
    bool instruction_complete() const { return timing_.instruction_complete(); }
    std::uint64_t clock_pulses() const { return timing_.clock_pulses(); }
    std::uint64_t completed_instructions() const { return timing_.completed_instructions(); }
    void on_clock_pulse(control_unit::Phase next_phase) { timing_.on_clock_pulse(next_phase); }
    control_unit::ControlSignals signals(isa::Instruction instruction) const {
        return signal_generator_.generate(phase(), instruction);
    }
    control_unit::Phase next_phase(isa::Instruction instruction) const {
        return logic_.next_phase(phase(), instruction);
    }
private:
    static void validate_mode(Mode mode) {
        if (mode != Mode::Hardwired && mode != Mode::Microprogrammed)
            throw std::invalid_argument("unknown control mode");
    }
    control_unit::rv32::ControlInterface& active() {
        if (mode_ == Mode::Hardwired) return hardwired_;
        return microprogrammed_;
    }
    const control_unit::rv32::ControlInterface& active() const {
        if (mode_ == Mode::Hardwired) return hardwired_;
        return microprogrammed_;
    }
    Mode mode_;
    control_unit::HardwiredController hardwired_;
    control_unit::MicroprogrammedController microprogrammed_;
    std::uint64_t rv32_pulses_ = 0, rv32_retired_ = 0;
    control_unit::TimingGenerator timing_;
    control_unit::ControlLogic logic_;
    control_unit::ControlSignalGenerator signal_generator_;
};
}
