#pragma once

#include "../control_interface.hpp"

namespace colab::soc::core::control_unit::microprogrammed {

using MicroAddress = std::uint16_t;
enum class Sequence { Increment, Jump, Dispatch, IfEqual };

// 操作控制字段 + 顺序控制字段。phase 只用于日志/阶段统计，不参与控制信号生成。
struct Microinstruction {
    rv32::ControlSignals control;
    Sequence sequence = Sequence::Increment;
    MicroAddress next_address = 0;
    MicroAddress alternate_address = 0;
    rv32::Phase phase = rv32::Phase::FetchAddress;
};
} // namespace colab::soc::core::control_unit::microprogrammed
