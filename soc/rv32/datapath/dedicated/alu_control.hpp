#pragma once

namespace colab::soc::core::datapath::dedicated {
// Circuit operations, independent of instruction encoding. RV32 ADD/SUB use
// Addu/Subu for modulo-2^32 results; signed overflow is not an RV32 trap.
enum class ALUCtr { Add, Addu, Sub, Subu, And, Or, Xor, Nor, Slt, Sltu, Sll, Srl, Sra,
                    Mul, Mulh, Mulhsu, Mulhu, Div, Divu, Rem, Remu };
}
