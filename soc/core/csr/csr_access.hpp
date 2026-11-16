#pragma once

#include "../decoder/instruction.hpp"

namespace colab::soc::core {
// Shared CSR read/modify/write control for both execution engines. The backend
// checks privilege/existence and owns WARL fields; suppressed accesses never call it.
template<class RegisterFile>
std::uint32_t access_csr(RegisterFile& registers, const DecodedInstruction& instruction, std::uint32_t source_value) {
    const auto op = instruction.operation;
    if (!is_csr_operation(op)) throw std::logic_error("non-CSR operation sent to CSR port");
    const bool swap = op == Operation::Csrrw || op == Operation::Csrrwi;
    const bool clear = op == Operation::Csrrc || op == Operation::Csrrci;
    const bool read = !swap || instruction.rd != 0;
    const bool write = swap || instruction.rs1 != 0;
    const auto operand = is_csr_immediate(op) ? std::uint32_t(instruction.rs1) : source_value;
    const auto csr = static_cast<std::uint16_t>(instruction.raw >> 20);
    registers.check_guest_csr(csr, write);
    const auto before = read ? registers.read(csr) : 0u;
    if (write) registers.write(csr, swap ? operand : clear ? before & ~operand : before | operand);
    return before;
}
} // namespace colab::soc::core
