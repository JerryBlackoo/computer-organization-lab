#pragma once

#include <cstdint>
#include <stdexcept>

namespace colab::soc::core::control_unit {

// 指令的功能阶段。一个阶段可占多拍；不同指令可跳过不需要的阶段。
// MEM 专指执行指令时的数据访存，IF 中读取指令仍属于取指阶段。
enum class InstructionStage { Fetch, Decode, Execute, Memory, WriteBack };

inline const char* stage_name(InstructionStage stage) {
    switch (stage) {
    case InstructionStage::Fetch: return "IF";
    case InstructionStage::Decode: return "ID";
    case InstructionStage::Execute: return "EX";
    case InstructionStage::Memory: return "MEM";
    case InstructionStage::WriteBack: return "WB";
    }
    return "UNKNOWN";
}

// Phase 保留原有接口名，表示一拍内的微操作状态，不是上面的指令阶段。
// 一个 tick 对应一个时钟周期（节拍），其末尾的有效时钟脉冲统一锁存。
enum class Phase {
    FetchAddress, FetchRead, FetchWait1, FetchWait2, FetchWait3, FetchWait4, FetchLatch,
    Decode, ReadOperand, Execute, WriteBack, Halt
};

inline const char* phase_name(Phase phase) {
    switch (phase) {
    case Phase::FetchAddress: return "FETCH_ADDRESS";
    case Phase::FetchRead: return "FETCH_READ";
    case Phase::FetchWait1: return "FETCH_WAIT_1";
    case Phase::FetchWait2: return "FETCH_WAIT_2";
    case Phase::FetchWait3: return "FETCH_WAIT_3";
    case Phase::FetchWait4: return "FETCH_WAIT_4";
    case Phase::FetchLatch: return "FETCH_LATCH";
    case Phase::Decode: return "DECODE";
    case Phase::ReadOperand: return "READ_OPERAND";
    case Phase::Execute: return "EXECUTE";
    case Phase::WriteBack: return "WRITE_BACK";
    case Phase::Halt: return "HALT";
    }
    return "UNKNOWN";
}

inline InstructionStage instruction_stage(Phase phase) {
    switch (phase) {
    case Phase::FetchAddress:
    case Phase::FetchRead:
    case Phase::FetchWait1:
    case Phase::FetchWait2:
    case Phase::FetchWait3:
    case Phase::FetchWait4:
    case Phase::FetchLatch:
        return InstructionStage::Fetch;
    case Phase::Decode:
    case Phase::ReadOperand:
        return InstructionStage::Decode;
    case Phase::Execute:
    case Phase::Halt:
        return InstructionStage::Execute;
    case Phase::WriteBack:
        return InstructionStage::WriteBack;
    }
    throw std::logic_error("unknown CPU phase");
}

// 描述将由下一个有效时钟脉冲提交的这一拍；所有编号从 1 开始。
struct TimingState {
    std::uint64_t clock_pulse;
    std::uint64_t instruction_cycle;
    unsigned instruction_beat;
    InstructionStage stage;
    unsigned stage_beat;
};

class TimingGenerator {
public:
    Phase phase() const { return phase_; }
    InstructionStage stage() const { return instruction_stage(phase_); }
    TimingState timing() const {
        return {clock_pulses_ + 1, completed_instructions_ + 1,
                instruction_beat_, stage(), stage_beat_};
    }
    bool instruction_complete() const {
        return phase_ == Phase::WriteBack || phase_ == Phase::Halt;
    }
    std::uint64_t clock_pulses() const { return clock_pulses_; }
    std::uint64_t completed_instructions() const { return completed_instructions_; }

    // 只在本拍成功提交的有效时钟边沿调用；等待拍也消耗脉冲，异常不推进。
    void on_clock_pulse(Phase next_phase) {
        const auto next_stage = instruction_stage(next_phase);
        const auto current_stage = stage();
        ++clock_pulses_;
        if (instruction_complete()) {
            ++completed_instructions_;
            instruction_beat_ = 1;
            stage_beat_ = 1;
        } else {
            ++instruction_beat_;
            stage_beat_ = next_stage == current_stage ? stage_beat_ + 1 : 1;
        }
        phase_ = next_phase;
    }

private:
    Phase phase_ = Phase::FetchAddress;
    std::uint64_t clock_pulses_ = 0;
    std::uint64_t completed_instructions_ = 0;
    unsigned instruction_beat_ = 1;
    unsigned stage_beat_ = 1;
};
}

namespace colab::soc::core::control_unit::rv32 {

// 原有时序模块的 RV32 多周期状态，与 16 位教学 Core 的 Phase 互不混用。
enum class Phase {
    FetchAddress, FetchRead, FetchWait1, FetchWait2, FetchWait3, FetchWait4, FetchLatch,
    Dispatch, ReadOperands, Execute, LoadRead, LoadWait1, LoadWait2, LoadWait3, LoadWait4,
    LoadWriteBack, StoreWrite, BranchTest, Branch, WriteBack
};

inline InstructionStage instruction_stage(Phase phase) {
    switch (phase) {
    case Phase::FetchAddress: case Phase::FetchRead: case Phase::FetchWait1:
    case Phase::FetchWait2: case Phase::FetchWait3: case Phase::FetchWait4: case Phase::FetchLatch:
        return InstructionStage::Fetch;
    case Phase::Dispatch: case Phase::ReadOperands: return InstructionStage::Decode;
    case Phase::LoadRead: case Phase::LoadWait1: case Phase::LoadWait2:
    case Phase::LoadWait3: case Phase::LoadWait4: case Phase::StoreWrite:
        return InstructionStage::Memory;
    case Phase::LoadWriteBack: case Phase::WriteBack: return InstructionStage::WriteBack;
    case Phase::Execute: case Phase::BranchTest: case Phase::Branch: return InstructionStage::Execute;
    }
    throw std::logic_error("unknown RV32 phase");
}

class TimingGenerator {
public:
    Phase phase() const { return phase_; }
    std::uint64_t clock_pulses() const { return pulses_; }
    void abort_instruction() {
        ++pulses_; phase_ = Phase::FetchAddress; beat_ = 1; stage_beat_ = 1;
    }
    std::uint64_t completed_instructions() const { return retired_; }
    TimingState timing() const {
        return {pulses_ + 1, retired_ + 1, beat_, instruction_stage(phase_), stage_beat_};
    }
    void on_clock_pulse(Phase next, bool retire) {
        const bool same_stage = instruction_stage(next) == instruction_stage(phase_);
        ++pulses_;
        if (retire) ++retired_;
        beat_ = retire ? 1 : beat_ + 1;
        stage_beat_ = retire || !same_stage ? 1 : stage_beat_ + 1;
        phase_ = next;
    }
private:
    Phase phase_ = Phase::FetchAddress;
    std::uint64_t pulses_ = 0, retired_ = 0;
    unsigned beat_ = 1, stage_beat_ = 1;
};
} // namespace colab::soc::core::control_unit::rv32
