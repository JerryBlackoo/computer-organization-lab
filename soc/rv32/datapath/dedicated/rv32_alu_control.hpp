#pragma once

#include "alu_control.hpp"
#include "../../../shared/decoder/instruction.hpp"

namespace colab::soc::core::datapath::dedicated {
inline ALUCtr rv32_alu_control(Operation operation) {
    switch (operation) {
    case Operation::Add: case Operation::Addi: case Operation::Lw: case Operation::Sw:
    case Operation::Lb: case Operation::Lbu: case Operation::Lh: case Operation::Lhu:
    case Operation::Sb: case Operation::Sh: return ALUCtr::Addu;
    case Operation::Sub: return ALUCtr::Subu;
    case Operation::Mul: return ALUCtr::Mul;
    case Operation::Mulh: return ALUCtr::Mulh;
    case Operation::Mulhsu: return ALUCtr::Mulhsu;
    case Operation::Mulhu: return ALUCtr::Mulhu;
    case Operation::Div: return ALUCtr::Div;
    case Operation::Divu: return ALUCtr::Divu;
    case Operation::Rem: return ALUCtr::Rem;
    case Operation::Remu: return ALUCtr::Remu;
    case Operation::And: case Operation::Andi: return ALUCtr::And;
    case Operation::Or: case Operation::Ori: return ALUCtr::Or;
    case Operation::Xor: case Operation::Xori: return ALUCtr::Xor;
    case Operation::Sll: case Operation::Slli: return ALUCtr::Sll;
    case Operation::Srl: case Operation::Srli: return ALUCtr::Srl;
    case Operation::Sra: case Operation::Srai: return ALUCtr::Sra;
    case Operation::Slt: case Operation::Slti: return ALUCtr::Slt;
    case Operation::Sltu: case Operation::Sltiu: return ALUCtr::Sltu;
    default: throw std::invalid_argument("operation does not select an RV32 ALU function");
    }
}
} // namespace colab::soc::core::datapath::dedicated
