#pragma once

#include "../timing_generator/timing_generator.hpp"
#include "../control_signals.hpp"
#include "../../decoder/instruction.hpp"
#include "../../datapath/dedicated/rv32_alu_control.hpp"

namespace colab::soc::core::control_unit {

class ControlSignalGenerator {
public:
    rv32::ControlSignals generate(rv32::Phase phase, const DecodedInstruction& instruction,
                                 rv32::Feedback feedback) const {
        using P = rv32::Phase;
        using datapath::dedicated::ALUCtr;
        rv32::ControlSignals signals;
        signals.memory_bytes = memory_access_bytes(instruction.operation);
        signals.load_signed = signed_load(instruction.operation);
        switch (phase) {
        case P::FetchAddress: signals.mar_from_pc = true; break;
        case P::FetchRead: case P::FetchWait1: case P::FetchWait2: case P::FetchWait3:
            signals.instruction_read = true; break;
        case P::FetchWait4:
            signals.instruction_read = true;
            signals.instruction_to_mdr = true;
            break;
        case P::FetchLatch: signals.ir_in = true; signals.decode = true; break;
        case P::ReadOperands: signals.operands_in = true; break;
        case P::Execute:
            if (is_system_operation(instruction.operation) || instruction.operation == Operation::Lui ||
                instruction.operation == Operation::Auipc || instruction.operation == Operation::Jal ||
                instruction.operation == Operation::Jalr) break;
            signals.operand_b = is_alu_immediate(instruction.operation) ||
                is_load(instruction.operation) || is_store(instruction.operation)
                ? rv32::OperandB::Immediate : rv32::OperandB::Register;
            signals.alu_op = is_conditional_branch(instruction.operation)
                ? ALUCtr::Subu : datapath::dedicated::rv32_alu_control(instruction.operation);
            if (is_muldiv(instruction.operation)) {
                signals.muldiv_start = !feedback.muldiv_busy;
                signals.muldiv_step = feedback.muldiv_busy;
                break;
            }
            signals.condition_in = is_conditional_branch(instruction.operation);
            switch (instruction.operation) {
            case Operation::Bne: signals.compare = rv32::Compare::NotEqual; break;
            case Operation::Blt: signals.compare = rv32::Compare::LessSigned; break;
            case Operation::Bge: signals.compare = rv32::Compare::GreaterEqualSigned; break;
            case Operation::Bltu: signals.compare = rv32::Compare::LessUnsigned; break;
            case Operation::Bgeu: signals.compare = rv32::Compare::GreaterEqualUnsigned; break;
            default: break;
            }
            signals.alu_in = !signals.condition_in;
            signals.address_in = is_load(instruction.operation) || is_store(instruction.operation);
            break;
        case P::LoadRead: case P::LoadWait1: case P::LoadWait2: case P::LoadWait3:
            signals.data_read = true; break;
        case P::LoadWait4: signals.data_read = true; signals.data_to_mdr = true; break;
        case P::LoadWriteBack:
            signals.write_back = rv32::WriteBack::Memory;
            signals.register_write = true; signals.pc_write = true; signals.retire = true;
            break;
        case P::StoreWrite:
            signals.memory_write = true; signals.pc_write = true; signals.retire = true;
            break;
        case P::Branch:
            signals.pc_source = feedback.equal ? rv32::PCSource::Branch : rv32::PCSource::Sequential;
            signals.pc_write = true; signals.retire = true;
            break;
        case P::WriteBack:
            signals.system = is_system_operation(instruction.operation);
            signals.register_write = !signals.system; signals.pc_write = true; signals.retire = true;
            if (instruction.operation == Operation::Lui) signals.write_back = rv32::WriteBack::UpperImmediate;
            else if (instruction.operation == Operation::Auipc) signals.write_back = rv32::WriteBack::PcRelative;
            else if (instruction.operation == Operation::Jal || instruction.operation == Operation::Jalr) {
                signals.write_back = rv32::WriteBack::ReturnAddress;
                signals.pc_source = instruction.operation == Operation::Jal ? rv32::PCSource::JumpImmediate
                                                                             : rv32::PCSource::JumpRegister;
            }
            break;
        case P::Dispatch: case P::BranchTest:
            throw std::logic_error("invalid hardwired RV32 control phase");
        }
        return signals;
    }

    ControlSignals generate(Phase phase, isa::Instruction instruction) const {
        using datapath::BusSource;
        ControlSignals signals;
        switch (phase) {
        case Phase::FetchAddress:
            signals.bus_source = BusSource::PC;
            signals.mar_in = true;
            break;
        case Phase::FetchRead:
        case Phase::FetchWait1:
        case Phase::FetchWait2:
        case Phase::FetchWait3:
            signals.read = true;
            signals.mdr_in = true;
            break;
        case Phase::FetchWait4:
            signals.read = true;
            signals.mdr_in = true;
            signals.memory_to_mdr = true;
            break;
        case Phase::FetchLatch:
            signals.bus_source = BusSource::MDR;
            signals.ir_in = true;
            signals.increment_pc = true;
            signals.decode = true;
            break;
        case Phase::Decode:
            signals.decode = true;
            break;
        case Phase::ReadOperand:
            signals.bus_source = BusSource::GPR;
            signals.register_select = RegisterSelect::Rs1;
            signals.y_in = true;
            break;
        case Phase::Execute:
            signals.bus_source = BusSource::GPR;
            signals.register_select = RegisterSelect::Rs2;
            signals.alu_op = instruction.op == isa::Opcode::Sub
                ? ALUOperation::Subtract : ALUOperation::Add;
            signals.z_in = true;
            signals.fr_in = true;
            break;
        case Phase::WriteBack:
            signals.bus_source = BusSource::Z;
            signals.gpr_in = true;
            break;
        case Phase::Halt:
            signals.halt = true;
            break;
        }
        return signals;
    }
};

} // namespace colab::soc::core::control_unit
