#pragma once

#include "control_store.hpp"

namespace colab::soc::core::control_unit::microprogrammed {

// 微地址形成部件：μPC+1、显式下址、IR 分派以及状态标志条件转移。
class Microsequencer {
public:
    MicroAddress address() const { return micro_pc_; }
    void latch(MicroAddress next) { micro_pc_ = next; }
    MicroAddress next(const Microinstruction& instruction, const DecodedInstruction& decoded,
                      rv32::Feedback flags, const ControlStore& store) const {
        switch (instruction.sequence) {
        case Sequence::Increment: return micro_pc_ + 1;
        case Sequence::Jump: return instruction.next_address;
        case Sequence::Dispatch: return store.entry(decoded.operation);
        case Sequence::IfEqual:
            return flags.equal ? instruction.next_address : instruction.alternate_address;
        case Sequence::IfMulDivDone:
            return flags.muldiv_done ? instruction.next_address : instruction.alternate_address;
        }
        throw std::logic_error("invalid microinstruction sequence field");
    }
private:
    MicroAddress micro_pc_ = ControlStore::fetch_entry;
};
} // namespace colab::soc::core::control_unit::microprogrammed
