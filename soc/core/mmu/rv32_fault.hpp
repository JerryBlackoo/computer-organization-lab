#pragma once

#include "types.hpp"
#include "../datapath/rv32.hpp"

namespace colab::soc::core {
// The teaching page table remains distinct from Sv32. Preserve access type and
// virtual address when delivering its failures to a machine trap handler.
inline RV32Fault rv32_translation_fault(const mmu::TranslationFault& fault, std::uint32_t pc) {
    const bool access = fault.reason() == mmu::FaultReason::PageTableAccess;
    RV32FaultCause cause;
    switch (fault.access()) {
    case mmu::AccessType::Execute:
        cause = access ? RV32FaultCause::InstructionAccessFault : RV32FaultCause::InstructionPageFault; break;
    case mmu::AccessType::Read:
        cause = access ? RV32FaultCause::LoadAccessFault : RV32FaultCause::LoadPageFault; break;
    case mmu::AccessType::Write:
        cause = access ? RV32FaultCause::StoreAccessFault : RV32FaultCause::StorePageFault; break;
    default: throw std::logic_error("unknown translation access type");
    }
    return RV32Fault(cause, pc, fault.address());
}
} // namespace colab::soc::core
