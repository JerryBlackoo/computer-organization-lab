#pragma once

#include "control_signals.hpp"
#include "timing_generator/timing_generator.hpp"
#include "../decoder/instruction.hpp"

namespace colab::soc::core::control_unit {

enum class ControlMode { Hardwired, Microprogrammed };

namespace rv32 {
// signals 是本拍组合输出，clock 只在数据通路成功提交的边沿推进控制器。
class ControlInterface {
public:
    virtual ~ControlInterface() = default;
    virtual void reset() = 0;
    virtual void abort_instruction() = 0;
    virtual ControlSignals signals(const DecodedInstruction&, Feedback) const = 0;
    virtual void clock(const DecodedInstruction&, Feedback) = 0;
    virtual Phase phase() const = 0;
    virtual TimingState timing() const = 0;
    virtual bool instruction_complete() const = 0;
    virtual bool at_instruction_boundary() const = 0;
    virtual std::uint64_t clock_pulses() const = 0;
    virtual std::uint64_t completed_instructions() const = 0;
};
} // namespace rv32
} // namespace colab::soc::core::control_unit
