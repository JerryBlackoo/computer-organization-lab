#pragma once
#include "instruction.hpp"
#include <cstdint>
#include <stdexcept>

namespace colab::soc::core {
class Decoder {
public:
    DecodedInstruction decode_rv32(std::uint32_t word) const {
        DecodedInstruction instruction;
        instruction.raw = word;
        instruction.opcode = word & 0x7Fu;
        instruction.rd = (word >> 7) & 31u;
        instruction.funct3 = (word >> 12) & 7u;
        instruction.rs1 = (word >> 15) & 31u;
        instruction.rs2 = (word >> 20) & 31u;
        instruction.funct7 = word >> 25;
        if (word == 0x10500073u) { instruction.operation = Operation::Wfi; return instruction; }
        if (word == 0x10200073u) { instruction.operation = Operation::Sret; return instruction; }
        if ((word & 0xfe007fffu) == 0x12000073u) { instruction.operation = Operation::SfenceVma; return instruction; }
        if (word == 0x30200073u || word == 0x00000073u || word == 0x00100073u) {
            instruction.operation = word == 0x30200073u ? Operation::Mret :
                word == 0x00000073u ? Operation::Ecall : Operation::Ebreak;
            return instruction;
        }
        if (instruction.opcode == 0x37 || instruction.opcode == 0x17) {
            instruction.operation = instruction.opcode == 0x37 ? Operation::Lui : Operation::Auipc;
            instruction.immediate = sign_extend(word >> 12, 20) * 4096;
            return instruction;
        }
        if (instruction.opcode == 0x6f) {
            instruction.operation = Operation::Jal;
            const auto immediate = ((word >> 31) << 20) | (((word >> 12) & 0xffu) << 12) |
                (((word >> 20) & 1u) << 11) | (((word >> 21) & 0x3ffu) << 1);
            instruction.immediate = sign_extend(immediate, 21);
            return instruction;
        }
        if (instruction.opcode == 0x67 && instruction.funct3 == 0) {
            instruction.operation = Operation::Jalr;
            instruction.immediate = sign_extend(word >> 20, 12);
            return instruction;
        }
        if (instruction.opcode == 0x0f && (instruction.funct3 == 0 || instruction.funct3 == 1)) {
            // Conservative full fence; reserved fm/pred/succ and unused rs1/rd
            // fields must not create register dependencies or writes.
            instruction.operation = instruction.funct3 == 0 ? Operation::Fence : Operation::FenceI;
            return instruction;
        }
        if (instruction.opcode == 0x73 && instruction.funct3 != 0 && instruction.funct3 != 4) {
            switch (instruction.funct3) {
            case 1: instruction.operation = Operation::Csrrw; break;
            case 2: instruction.operation = Operation::Csrrs; break;
            case 3: instruction.operation = Operation::Csrrc; break;
            case 5: instruction.operation = Operation::Csrrwi; break;
            case 6: instruction.operation = Operation::Csrrsi; break;
            case 7: instruction.operation = Operation::Csrrci; break;
            }
            if (is_csr_immediate(instruction.operation)) instruction.immediate = instruction.rs1;
            return instruction;
        }
        if (instruction.opcode == 0x33) {
            if (instruction.funct7 == 0x01) {
                switch (instruction.funct3) {
                case 0: instruction.operation = Operation::Mul; break;
                case 1: instruction.operation = Operation::Mulh; break;
                case 2: instruction.operation = Operation::Mulhsu; break;
                case 3: instruction.operation = Operation::Mulhu; break;
                case 4: instruction.operation = Operation::Div; break;
                case 5: instruction.operation = Operation::Divu; break;
                case 6: instruction.operation = Operation::Rem; break;
                case 7: instruction.operation = Operation::Remu; break;
                }
                return instruction;
            }
            if (instruction.funct7 != 0 && !(instruction.funct7 == 0x20 &&
                (instruction.funct3 == 0 || instruction.funct3 == 5)))
                throw std::invalid_argument("unsupported RV32 R-type instruction");
            switch (instruction.funct3) {
            case 0: instruction.operation = instruction.funct7 ? Operation::Sub : Operation::Add; break;
            case 1: instruction.operation = Operation::Sll; break;
            case 2: instruction.operation = Operation::Slt; break;
            case 3: instruction.operation = Operation::Sltu; break;
            case 4: instruction.operation = Operation::Xor; break;
            case 5: instruction.operation = instruction.funct7 ? Operation::Sra : Operation::Srl; break;
            case 6: instruction.operation = Operation::Or; break;
            case 7: instruction.operation = Operation::And; break;
            }
            return instruction;
        }
        if (instruction.opcode == 0x13) {
            switch (instruction.funct3) {
            case 0: instruction.operation = Operation::Addi; break;
            case 1:
                if (instruction.funct7) throw std::invalid_argument("invalid RV32 SLLI encoding");
                instruction.operation = Operation::Slli; break;
            case 2: instruction.operation = Operation::Slti; break;
            case 3: instruction.operation = Operation::Sltiu; break;
            case 4: instruction.operation = Operation::Xori; break;
            case 5:
                if (instruction.funct7 != 0 && instruction.funct7 != 0x20)
                    throw std::invalid_argument("invalid RV32 right-shift encoding");
                instruction.operation = instruction.funct7 ? Operation::Srai : Operation::Srli; break;
            case 6: instruction.operation = Operation::Ori; break;
            case 7: instruction.operation = Operation::Andi; break;
            }
            instruction.immediate = instruction.funct3 == 1 || instruction.funct3 == 5
                ? static_cast<std::int32_t>((word >> 20) & 31u) : sign_extend(word >> 20, 12);
            return instruction;
        }
        if (instruction.opcode == 0x03) {
            switch (instruction.funct3) {
            case 0: instruction.operation = Operation::Lb; break;
            case 1: instruction.operation = Operation::Lh; break;
            case 2: instruction.operation = Operation::Lw; break;
            case 4: instruction.operation = Operation::Lbu; break;
            case 5: instruction.operation = Operation::Lhu; break;
            default: throw std::invalid_argument("reserved RV32 load encoding");
            }
            instruction.immediate = sign_extend(word >> 20, 12);
            return instruction;
        }
        if (instruction.opcode == 0x23) {
            switch (instruction.funct3) {
            case 0: instruction.operation = Operation::Sb; break;
            case 1: instruction.operation = Operation::Sh; break;
            case 2: instruction.operation = Operation::Sw; break;
            default: throw std::invalid_argument("reserved RV32 store encoding");
            }
            instruction.immediate = sign_extend(((word >> 25) << 5) | ((word >> 7) & 31u), 12);
            return instruction;
        }
        if (instruction.opcode == 0x63) {
            switch (instruction.funct3) {
            case 0: instruction.operation = Operation::Beq; break;
            case 1: instruction.operation = Operation::Bne; break;
            case 4: instruction.operation = Operation::Blt; break;
            case 5: instruction.operation = Operation::Bge; break;
            case 6: instruction.operation = Operation::Bltu; break;
            case 7: instruction.operation = Operation::Bgeu; break;
            default: throw std::invalid_argument("unsupported RV32 branch encoding");
            }
            const auto immediate = ((word >> 31) << 12) | (((word >> 7) & 1u) << 11)
                | (((word >> 25) & 63u) << 5) | (((word >> 8) & 15u) << 1);
            instruction.immediate = sign_extend(immediate, 13);
            return instruction;
        }
        throw std::invalid_argument("unsupported RV32 instruction");
    }

    // 保留 16 位教学 Core 的源代码兼容性；RV32 明确使用 decode_rv32，避免隐式截断。
    isa::Instruction decode(std::uint16_t word) const {
        const auto op = static_cast<isa::Opcode>(word >> 12);
        switch (op) {
        case isa::Opcode::Add:
        case isa::Opcode::Sub:
            return {op, (word >> 8) & 0xFu, (word >> 4) & 0xFu, word & 0xFu};
        case isa::Opcode::Halt:
            if (word == 0xF000) return {op};
            break;
        }
        throw std::invalid_argument("unknown instruction opcode");
    }
private:
    static std::int32_t sign_extend(std::uint32_t value, unsigned bits) {
        // 在可表示范围内做有符号减法，不依赖 unsigned -> signed 越界转换。
        return static_cast<std::int32_t>(value & ((1u << bits) - 1u))
            - ((value & (1u << (bits - 1))) ? static_cast<std::int32_t>(1u << bits) : 0);
    }
};
}
