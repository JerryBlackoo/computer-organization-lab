#pragma once

#include "../timing_generator/timing_generator.hpp"
#include "../../decoder/instruction.hpp"

#include <stdexcept>

namespace colab::soc::core::control_unit {

class ControlLogic {
public:
    rv32::Phase next_phase(rv32::Phase phase, const DecodedInstruction& instruction) const {
        using P = rv32::Phase;
        switch (phase) {
        case P::FetchAddress: return P::FetchRead;
        case P::FetchRead: return P::FetchWait1;
        case P::FetchWait1: return P::FetchWait2;
        case P::FetchWait2: return P::FetchWait3;
        case P::FetchWait3: return P::FetchWait4;
        case P::FetchWait4: return P::FetchLatch;
        case P::FetchLatch: return P::ReadOperands;
        case P::ReadOperands: return P::Execute;
        case P::Execute:
            switch (instruction.operation) {
            case Operation::Lw: case Operation::Lb: case Operation::Lbu:
            case Operation::Lh: case Operation::Lhu: return P::LoadRead;
            case Operation::Sw: case Operation::Sb: case Operation::Sh: return P::StoreWrite;
            case Operation::Beq: case Operation::Bne: case Operation::Blt: case Operation::Bge:
            case Operation::Bltu: case Operation::Bgeu: return P::Branch;
            default: return P::WriteBack;
            }
        case P::LoadRead: return P::LoadWait1;
        case P::LoadWait1: return P::LoadWait2;
        case P::LoadWait2: return P::LoadWait3;
        case P::LoadWait3: return P::LoadWait4;
        case P::LoadWait4: return P::LoadWriteBack;
        case P::LoadWriteBack: case P::StoreWrite: case P::Branch: case P::WriteBack:
            return P::FetchAddress;
        case P::Dispatch: case P::BranchTest: break; // 仅由微程序使用的状态标签。
        }
        throw std::logic_error("invalid hardwired RV32 state");
    }

    Phase next_phase(Phase phase, isa::Instruction instruction) const {
        switch (phase) {
        // 图 5.6：C1 送地址，C2~C6 主存读（五拍），C7 MDR -> IR。
        case Phase::FetchAddress: return Phase::FetchRead;
        case Phase::FetchRead: return Phase::FetchWait1;
        case Phase::FetchWait1: return Phase::FetchWait2;
        case Phase::FetchWait2: return Phase::FetchWait3;
        case Phase::FetchWait3: return Phase::FetchWait4;
        case Phase::FetchWait4: return Phase::FetchLatch;
        // C7 锁存 IR 后组合译码，直接准备 C8 的操作数控制，不另插译码拍。
        case Phase::FetchLatch:
        case Phase::Decode:
            return instruction.op == isa::Opcode::Halt ? Phase::Halt : Phase::ReadOperand;
        case Phase::ReadOperand: return Phase::Execute;
        // EX -> WB：ADD/SUB 不访问数据存储器，跳过 MEM。
        case Phase::Execute: return Phase::WriteBack;
        // 完成当前指令后，才开始下一条指令的 IF；这里没有流水重叠。
        case Phase::WriteBack: return Phase::FetchAddress;
        case Phase::Halt: return Phase::Halt;
        }
        throw std::logic_error("unknown CPU phase");
    }
};
}
