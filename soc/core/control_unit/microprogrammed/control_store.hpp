#pragma once

#include "microinstruction.hpp"
#include "../../datapath/dedicated/rv32_alu_control.hpp"

#include <array>
#include <optional>

namespace colab::soc::core::control_unit::microprogrammed {

// 独立于程序/数据 RAM 的控制存储器；构造时装载，此后由控制器只读使用。
class ControlStore {
public:
    static constexpr std::size_t capacity = 256;
    static constexpr MicroAddress fetch_entry = 0;

    ControlStore() {
        using P = rv32::Phase;
        using datapath::dedicated::ALUCtr;
        auto& address = put(0, P::FetchAddress); address.control.mar_from_pc = true;
        for (MicroAddress i = 1; i <= 5; ++i) {
            const P phases[]{P::FetchRead, P::FetchWait1, P::FetchWait2, P::FetchWait3, P::FetchWait4};
            auto& read = put(i, phases[i - 1]);
            read.control.instruction_read = true;
            read.control.instruction_to_mdr = i == 5;
        }
        put(6, P::FetchLatch).control.ir_in = true;
        auto& dispatch = put(7, P::Dispatch);
        dispatch.control.decode = true;
        dispatch.sequence = Sequence::Dispatch;

        // 每条机器指令各有入口；相容使能在一条水平微指令内并发。
        arithmetic(16, rv32::OperandB::Register, ALUCtr::Addu); // ADD
        arithmetic(20, rv32::OperandB::Immediate, ALUCtr::Addu); // ADDI
        arithmetic(24, rv32::OperandB::Register, ALUCtr::Subu); // SUB

        put(28, P::ReadOperands).control.operands_in = true;
        effective_address(29);
        for (MicroAddress i = 30; i <= 34; ++i) {
            const P phases[]{P::LoadRead, P::LoadWait1, P::LoadWait2, P::LoadWait3, P::LoadWait4};
            auto& read = put(i, phases[i - 30]);
            read.control.data_read = true;
            read.control.data_to_mdr = i == 34;
        }
        auto& load = finish(35, P::LoadWriteBack);
        load.control.register_write = true;
        load.control.write_back = rv32::WriteBack::Memory;

        put(36, P::ReadOperands).control.operands_in = true;
        effective_address(37);
        finish(38, P::StoreWrite).control.memory_write = true;

        put(40, P::ReadOperands).control.operands_in = true;
        auto& compare = put(41, P::Execute);
        compare.control.alu_op = ALUCtr::Subu;
        compare.control.condition_in = true;
        auto& test = put(42, P::BranchTest);
        test.sequence = Sequence::IfEqual;
        test.next_address = 43;
        test.alternate_address = 44;
        finish(43, P::Branch).control.pc_source = rv32::PCSource::Branch;
        finish(44, P::Branch); // 条件不成立，顺序 PC。

        put(48, P::ReadOperands).control.operands_in = true;
        put(49, P::Execute);
        finish(50, P::WriteBack).control.system = true;

        auto& lui = finish(52, P::WriteBack); lui.control.register_write = true;
        lui.control.write_back = rv32::WriteBack::UpperImmediate;
        auto& auipc = finish(53, P::WriteBack); auipc.control.register_write = true;
        auipc.control.write_back = rv32::WriteBack::PcRelative;
        auto& jal = finish(54, P::WriteBack); jal.control.register_write = true;
        jal.control.write_back = rv32::WriteBack::ReturnAddress; jal.control.pc_source = rv32::PCSource::JumpImmediate;
        put(55, P::ReadOperands).control.operands_in = true;
        auto& jalr = finish(56, P::WriteBack); jalr.control.register_write = true;
        jalr.control.write_back = rv32::WriteBack::ReturnAddress; jalr.control.pc_source = rv32::PCSource::JumpRegister;
        branch(64, rv32::Compare::NotEqual);
        branch(69, rv32::Compare::LessSigned);
        branch(74, rv32::Compare::GreaterEqualSigned);
        branch(79, rv32::Compare::LessUnsigned);
        branch(84, rv32::Compare::GreaterEqualUnsigned);
        for (std::size_t i = 0; i < extended_arithmetic.size(); ++i) {
            const auto operation = extended_arithmetic[i];
            arithmetic(static_cast<MicroAddress>(96 + i * 3),
                is_alu_immediate(operation) ? rv32::OperandB::Immediate : rv32::OperandB::Register,
                datapath::dedicated::rv32_alu_control(operation));
        }
        memory_sequence(160, 28, 8, 1, true);  // LB
        memory_sequence(168, 28, 8, 1, false); // LBU
        memory_sequence(176, 28, 8, 2, true);  // LH
        memory_sequence(184, 28, 8, 2, false); // LHU
        memory_sequence(192, 36, 3, 1, false); // SB
        memory_sequence(195, 36, 3, 2, false); // SH
    }

    const Microinstruction& at(MicroAddress address) const {
        const auto& word = words_.at(address);
        if (!word) throw std::logic_error("uninitialized control-store address");
        return *word;
    }
    // 仅供构造实验 ROM 映像；控制器持有自己的副本，不允许运行时修改。
    void set(MicroAddress address, Microinstruction instruction) { words_.at(address) = instruction; }

    MicroAddress entry(Operation operation) const {
        switch (operation) {
        case Operation::Add: return 16;
        case Operation::Addi: return 20;
        case Operation::Sub: return 24;
        case Operation::Lw: return 28;
        case Operation::Sw: return 36;
        case Operation::Lb: return 160;
        case Operation::Lbu: return 168;
        case Operation::Lh: return 176;
        case Operation::Lhu: return 184;
        case Operation::Sb: return 192;
        case Operation::Sh: return 195;
        case Operation::Beq: return 40;
        case Operation::Bne: return 64;
        case Operation::Blt: return 69;
        case Operation::Bge: return 74;
        case Operation::Bltu: return 79;
        case Operation::Bgeu: return 84;
        case Operation::Lui: return 52;
        case Operation::Auipc: return 53;
        case Operation::Jal: return 54;
        case Operation::Jalr: return 55;
        case Operation::Csrrw: case Operation::Csrrs: case Operation::Csrrc:
        case Operation::Csrrwi: case Operation::Csrrsi: case Operation::Csrrci:
        case Operation::Fence: case Operation::FenceI: case Operation::Mret: case Operation::Wfi:
        case Operation::Sret: case Operation::SfenceVma:
        case Operation::Ecall: case Operation::Ebreak: return 48;
        default:
            for (std::size_t i = 0; i < extended_arithmetic.size(); ++i)
                if (operation == extended_arithmetic[i]) return static_cast<MicroAddress>(96 + i * 3);
        }
        throw std::invalid_argument("no microprogram for operation");
    }
private:
    void memory_sequence(MicroAddress address, MicroAddress source, unsigned length, unsigned bytes, bool sign) {
        for (unsigned i = 0; i < length; ++i) {
            auto word = at(static_cast<MicroAddress>(source + i));
            word.control.memory_bytes = bytes;
            word.control.load_signed = sign;
            set(static_cast<MicroAddress>(address + i), word);
        }
    }
    static constexpr std::array<Operation, 16> extended_arithmetic{
        Operation::And, Operation::Or, Operation::Xor, Operation::Sll, Operation::Srl, Operation::Sra, Operation::Slt, Operation::Sltu,
        Operation::Andi, Operation::Ori, Operation::Xori, Operation::Slli, Operation::Srli, Operation::Srai, Operation::Slti, Operation::Sltiu};
    Microinstruction& put(MicroAddress address, rv32::Phase phase) {
        words_.at(address) = Microinstruction{};
        words_[address]->phase = phase;
        return *words_[address];
    }
    Microinstruction& finish(MicroAddress address, rv32::Phase phase) {
        auto& word = put(address, phase);
        word.control.pc_write = true;
        word.control.retire = true;
        word.sequence = Sequence::Jump;
        word.next_address = fetch_entry;
        return word;
    }
    void arithmetic(MicroAddress address, rv32::OperandB b, datapath::dedicated::ALUCtr op) {
        put(address, rv32::Phase::ReadOperands).control.operands_in = true;
        auto& execute = put(address + 1, rv32::Phase::Execute);
        execute.control.operand_b = b;
        execute.control.alu_op = op;
        execute.control.alu_in = true;
        finish(address + 2, rv32::Phase::WriteBack).control.register_write = true;
    }
    void effective_address(MicroAddress address) {
        auto& word = put(address, rv32::Phase::Execute);
        word.control.operand_b = rv32::OperandB::Immediate;
        word.control.alu_in = true;
        word.control.address_in = true;
    }
    void branch(MicroAddress address, rv32::Compare comparison) {
        put(address, rv32::Phase::ReadOperands).control.operands_in = true;
        auto& compare = put(address + 1, rv32::Phase::Execute);
        compare.control.condition_in = true; compare.control.compare = comparison;
        auto& test = put(address + 2, rv32::Phase::BranchTest);
        test.sequence = Sequence::IfEqual; test.next_address = address + 3; test.alternate_address = address + 4;
        finish(address + 3, rv32::Phase::Branch).control.pc_source = rv32::PCSource::Branch;
        finish(address + 4, rv32::Phase::Branch);
    }
    std::array<std::optional<Microinstruction>, capacity> words_{};
};
} // namespace colab::soc::core::control_unit::microprogrammed
