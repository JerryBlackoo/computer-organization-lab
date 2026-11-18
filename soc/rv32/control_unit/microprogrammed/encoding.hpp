#pragma once

#include "microinstruction.hpp"
#include <cstdint>
#include <stdexcept>

namespace colab::soc::core::control_unit::microprogrammed {
// Fixed, bit-addressed control word, independent of C++ structure layout.
// See docs/binary-microcode.md for the public bit layout.
using MicroWord = std::uint64_t;
inline constexpr unsigned micro_word_bits = 64;
// The exported binary format depends on these numeric selectors. Fail at compile
// time rather than silently changing the hardware ABI if an enum is reordered.
using EncodedALU = datapath::dedicated::ALUCtr;
static_assert(static_cast<unsigned>(EncodedALU::Add) == 0 && static_cast<unsigned>(EncodedALU::Addu) == 1 &&
    static_cast<unsigned>(EncodedALU::Sub) == 2 && static_cast<unsigned>(EncodedALU::Subu) == 3 &&
    static_cast<unsigned>(EncodedALU::And) == 4 && static_cast<unsigned>(EncodedALU::Or) == 5 &&
    static_cast<unsigned>(EncodedALU::Xor) == 6 && static_cast<unsigned>(EncodedALU::Nor) == 7 &&
    static_cast<unsigned>(EncodedALU::Slt) == 8 && static_cast<unsigned>(EncodedALU::Sltu) == 9 &&
    static_cast<unsigned>(EncodedALU::Sll) == 10 && static_cast<unsigned>(EncodedALU::Srl) == 11 &&
    static_cast<unsigned>(EncodedALU::Sra) == 12 && static_cast<unsigned>(EncodedALU::Mul) == 13 &&
    static_cast<unsigned>(EncodedALU::Mulh) == 14 && static_cast<unsigned>(EncodedALU::Mulhsu) == 15 &&
    static_cast<unsigned>(EncodedALU::Mulhu) == 16 && static_cast<unsigned>(EncodedALU::Div) == 17 &&
    static_cast<unsigned>(EncodedALU::Divu) == 18 && static_cast<unsigned>(EncodedALU::Rem) == 19 &&
    static_cast<unsigned>(EncodedALU::Remu) == 20, "microcode ALU encoding changed");
static_assert(static_cast<unsigned>(Sequence::Increment) == 0 && static_cast<unsigned>(Sequence::Jump) == 1 &&
    static_cast<unsigned>(Sequence::Dispatch) == 2 && static_cast<unsigned>(Sequence::IfEqual) == 3 &&
    static_cast<unsigned>(Sequence::IfMulDivDone) == 4, "microcode sequencer encoding changed");
static_assert(static_cast<unsigned>(rv32::OperandB::Register) == 0 && static_cast<unsigned>(rv32::OperandB::Immediate) == 1 &&
    static_cast<unsigned>(rv32::WriteBack::ALU) == 0 && static_cast<unsigned>(rv32::WriteBack::Memory) == 1 &&
    static_cast<unsigned>(rv32::WriteBack::UpperImmediate) == 2 && static_cast<unsigned>(rv32::WriteBack::PcRelative) == 3 &&
    static_cast<unsigned>(rv32::WriteBack::ReturnAddress) == 4 && static_cast<unsigned>(rv32::PCSource::Sequential) == 0 &&
    static_cast<unsigned>(rv32::PCSource::Branch) == 1 && static_cast<unsigned>(rv32::PCSource::JumpImmediate) == 2 &&
    static_cast<unsigned>(rv32::PCSource::JumpRegister) == 3 && static_cast<unsigned>(rv32::Compare::Equal) == 0 &&
    static_cast<unsigned>(rv32::Compare::NotEqual) == 1 && static_cast<unsigned>(rv32::Compare::LessSigned) == 2 &&
    static_cast<unsigned>(rv32::Compare::GreaterEqualSigned) == 3 && static_cast<unsigned>(rv32::Compare::LessUnsigned) == 4 &&
    static_cast<unsigned>(rv32::Compare::GreaterEqualUnsigned) == 5, "microcode selector encoding changed");

inline MicroWord encode(const Microinstruction& m) {
    using namespace rv32;
    const auto& c = m.control;
    MicroWord w = 0;
    const bool enables[] = {c.mar_from_pc, c.instruction_read, c.instruction_to_mdr,
        c.ir_in, c.decode, c.operands_in, c.muldiv_start, c.muldiv_step,
        c.alu_in, c.address_in, c.condition_in, c.data_read, c.data_to_mdr,
        c.memory_write, c.register_write, c.pc_write, c.retire, c.system};
    for (unsigned i = 0; i < 18; ++i) if (enables[i]) w |= MicroWord{1} << i;
    unsigned size;
    switch (c.memory_bytes) { case 4: size = 0; break; case 1: size = 1; break;
        case 2: size = 2; break; default: throw std::invalid_argument("invalid microcode memory width"); }
    auto field = [&w](unsigned value, unsigned max, unsigned shift) {
        if (value > max) throw std::invalid_argument("microinstruction field out of range");
        w |= MicroWord{value} << shift;
    };
    field(size, 2, 18); field(c.load_signed, 1, 20);
    field(static_cast<unsigned>(c.alu_op), 20, 21);
    field(static_cast<unsigned>(c.operand_b), 1, 26);
    field(static_cast<unsigned>(c.write_back), 4, 27);
    field(static_cast<unsigned>(c.pc_source), 3, 30);
    field(static_cast<unsigned>(c.compare), 5, 32);
    field(static_cast<unsigned>(m.sequence), 4, 35);
    field(m.next_address, 255, 38); field(m.alternate_address, 255, 46);
    return w;
}

inline Microinstruction decode(MicroWord w, rv32::Phase phase = rv32::Phase::FetchAddress) {
    using namespace rv32;
    using datapath::dedicated::ALUCtr;
    auto field = [w](unsigned shift, MicroWord mask) { return static_cast<unsigned>((w >> shift) & mask); };
    if ((w >> 54) || field(18, 3) == 3 || field(21, 31) > 20 ||
        field(27, 7) > 4 || field(32, 7) > 5 || field(35, 7) > 4)
        throw std::invalid_argument("invalid or reserved microinstruction encoding");
    Microinstruction m;
    auto& c = m.control;
    c.mar_from_pc = field(0, 1); c.instruction_read = field(1, 1);
    c.instruction_to_mdr = field(2, 1); c.ir_in = field(3, 1);
    c.decode = field(4, 1); c.operands_in = field(5, 1);
    c.muldiv_start = field(6, 1); c.muldiv_step = field(7, 1);
    c.alu_in = field(8, 1); c.address_in = field(9, 1);
    c.condition_in = field(10, 1); c.data_read = field(11, 1);
    c.data_to_mdr = field(12, 1); c.memory_write = field(13, 1);
    c.register_write = field(14, 1); c.pc_write = field(15, 1);
    c.retire = field(16, 1); c.system = field(17, 1);
    const auto size = field(18, 3); c.memory_bytes = size == 0 ? 4 : size == 1 ? 1 : 2;
    c.load_signed = field(20, 1); c.alu_op = static_cast<ALUCtr>(field(21, 31));
    c.operand_b = static_cast<OperandB>(field(26, 1));
    c.write_back = static_cast<WriteBack>(field(27, 7));
    c.pc_source = static_cast<PCSource>(field(30, 3));
    c.compare = static_cast<Compare>(field(32, 7));
    m.sequence = static_cast<Sequence>(field(35, 7));
    m.next_address = static_cast<MicroAddress>(field(38, 255));
    m.alternate_address = static_cast<MicroAddress>(field(46, 255));
    m.phase = phase;
    return m;
}
} // namespace colab::soc::core::control_unit::microprogrammed
