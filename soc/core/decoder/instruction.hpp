#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>

// 保留现有教学程序的接口；定义随译码器放在 soc 内，不依赖已移除的 isa 目录。
namespace colab::isa {
enum class Opcode { Add = 1, Sub = 2, Halt = 15 };

struct Instruction {
    Opcode op = Opcode::Halt;
    std::size_t rd = 0;
    std::size_t rs1 = 0;
    std::size_t rs2 = 0;
};

inline std::uint16_t encode(Instruction instruction) {
    if (instruction.rd >= 16 || instruction.rs1 >= 16 || instruction.rs2 >= 16)
        throw std::out_of_range("16-bit instruction register index");
    switch (instruction.op) {
    case Opcode::Add:
    case Opcode::Sub:
        return static_cast<std::uint16_t>((unsigned(instruction.op) << 12)
            | (instruction.rd << 8) | (instruction.rs1 << 4) | instruction.rs2);
    case Opcode::Halt:
        if (instruction.rd == 0 && instruction.rs1 == 0 && instruction.rs2 == 0) return 0xF000;
        break;
    }
    throw std::invalid_argument("invalid 16-bit teaching instruction");
}

} // namespace colab::isa

namespace colab::soc::core {

enum class Operation { Add, Sub, Addi, Lw, Sw, Beq, Bne, Blt, Bge, Bltu, Bgeu,
                       Lui, Auipc, Jal, Jalr, Csrrw, Csrrs, Mret,
                       And, Or, Xor, Sll, Srl, Sra, Slt, Sltu,
                       Andi, Ori, Xori, Slli, Srli, Srai, Slti, Sltiu,
                       Lb, Lbu, Lh, Lhu, Sb, Sh, Ecall, Ebreak,
                       Csrrc, Csrrwi, Csrrsi, Csrrci, Fence, FenceI, Wfi, Sret, SfenceVma };

inline bool is_csr_immediate(Operation op) {
    return op == Operation::Csrrwi || op == Operation::Csrrsi || op == Operation::Csrrci;
}
inline bool is_csr_operation(Operation op) {
    return op == Operation::Csrrw || op == Operation::Csrrs || op == Operation::Csrrc || is_csr_immediate(op);
}
inline bool is_fence(Operation op) { return op == Operation::Fence || op == Operation::FenceI; }

inline bool is_system_operation(Operation op) {
    return is_csr_operation(op) || is_fence(op) || op == Operation::Mret || op == Operation::Sret ||
        op == Operation::SfenceVma || op == Operation::Wfi ||
        op == Operation::Ecall || op == Operation::Ebreak;
}

inline bool is_load(Operation op) {
    return op == Operation::Lb || op == Operation::Lbu || op == Operation::Lh ||
        op == Operation::Lhu || op == Operation::Lw;
}
inline bool is_store(Operation op) { return op == Operation::Sb || op == Operation::Sh || op == Operation::Sw; }
inline unsigned memory_access_bytes(Operation op) {
    if (op == Operation::Lb || op == Operation::Lbu || op == Operation::Sb) return 1;
    if (op == Operation::Lh || op == Operation::Lhu || op == Operation::Sh) return 2;
    return 4;
}
inline bool signed_load(Operation op) { return op == Operation::Lb || op == Operation::Lh; }

inline bool is_alu_immediate(Operation op) {
    return op == Operation::Addi || op == Operation::Andi || op == Operation::Ori || op == Operation::Xori ||
        op == Operation::Slli || op == Operation::Srli || op == Operation::Srai || op == Operation::Slti || op == Operation::Sltiu;
}
inline bool is_register_alu(Operation op) {
    return op == Operation::Add || op == Operation::Sub || op == Operation::And || op == Operation::Or ||
        op == Operation::Xor || op == Operation::Sll || op == Operation::Srl || op == Operation::Sra ||
        op == Operation::Slt || op == Operation::Sltu;
}

inline bool is_conditional_branch(Operation op) {
    return op == Operation::Beq || op == Operation::Bne || op == Operation::Blt ||
        op == Operation::Bge || op == Operation::Bltu || op == Operation::Bgeu;
}

// 两套 RV32 控制器共同消费的组合译码结果；立即数已还原成有符号字节偏移。
struct DecodedInstruction {
    Operation operation = Operation::Add;
    std::uint8_t rd = 0, rs1 = 0, rs2 = 0;
    std::int32_t immediate = 0;
    std::uint8_t opcode = 0, funct3 = 0, funct7 = 0;
    std::uint32_t raw = 0;
};

} // namespace colab::soc::core
