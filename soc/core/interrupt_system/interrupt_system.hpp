#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include "../pmp/pmp.hpp"
#include "../datapath/rv32.hpp"

namespace colab::soc::core {
// RV32 M/S/U trap state. GPR preservation belongs to guest handlers.
// Sv32 uses the platform's 32-bit physical address width and nine ASID bits.
class InterruptSystem {
public:
    static constexpr std::uint32_t SIE = 1u << 1, SPIE = 1u << 5, SPP = 1u << 8;
    static constexpr std::uint32_t MIE = 1u << 3, MPIE = 1u << 7, MEIE = 1u << 11;
    static constexpr std::uint32_t MSIE = 1u << 3, MTIE = 1u << 7, LocalEnables = MSIE | MTIE | MEIE;
    static constexpr std::uint32_t SSIE = 1u << 1, STIE = 1u << 5, SEIE = 1u << 9;
    static constexpr std::uint32_t SupervisorEnables = SSIE | STIE | SEIE, AllEnables = LocalEnables | SupervisorEnables;
    static constexpr std::uint32_t MPP = 3u << 11, MPRV = 1u << 17;
    static constexpr std::uint32_t TVM = 1u << 20, TW = 1u << 21, TSR = 1u << 22;
    static constexpr std::uint32_t SUM = 1u << 18, MXR = 1u << 19;
    static constexpr std::uint32_t SupervisorStatus = SIE | SPIE | SPP | SUM | MXR;
    static constexpr std::uint32_t DelegableExceptions = 0x0000b3ffu;
    PrivilegeMode privilege() const { return privilege_; }
    PrivilegeMode data_privilege() const {
        return privilege_ == PrivilegeMode::Machine && (mstatus_ & MPRV) ? previous_privilege() : privilege_;
    }
    mmu::Privilege translation_privilege(bool instruction = false) const {
        return (instruction ? privilege_ : data_privilege()) == PrivilegeMode::User
            ? mmu::Privilege::User : mmu::Privilege::Supervisor;
    }
    bool sv32_enabled(mmu::AccessType access) const {
        return (satp_ & 0x80000000u) &&
            (access == mmu::AccessType::Execute ? privilege_ : data_privilege()) != PrivilegeMode::Machine;
    }
    bool page_table_read_allowed(std::uint32_t address) const {
        return pmp_.allows(address, 4, mmu::AccessType::Read, PrivilegeMode::Supervisor);
    }
    RV32FaultCause ecall_cause() const {
        return privilege_ == PrivilegeMode::User ? RV32FaultCause::EnvironmentCallFromU :
            privilege_ == PrivilegeMode::Supervisor ? RV32FaultCause::EnvironmentCallFromS : RV32FaultCause::EnvironmentCallFromM;
    }
    void check_memory(std::uint32_t address, unsigned bytes, mmu::AccessType access, std::uint32_t pc,
                      std::optional<std::uint32_t> virtual_address = std::nullopt) const {
        if (pmp_.allows(address, bytes, access, access == mmu::AccessType::Execute ? privilege_ : data_privilege())) return;
        const auto cause = access == mmu::AccessType::Execute ? RV32FaultCause::InstructionAccessFault :
            access == mmu::AccessType::Read ? RV32FaultCause::LoadAccessFault : RV32FaultCause::StoreAccessFault;
        throw RV32Fault(cause, pc, virtual_address.value_or(address));
    }
    void check_guest_csr(std::uint16_t csr, bool write) const {
        if (static_cast<unsigned>(privilege_) < ((csr >> 8) & 3u) || (write && (csr >> 10) == 3u) ||
            (csr == 0x180 && privilege_ == PrivilegeMode::Supervisor && (mstatus_ & TVM)))
            throw std::invalid_argument("insufficient CSR privilege or read-only CSR write");
    }
    void check_sfence_vma() const {
        if (privilege_ == PrivilegeMode::User || (privilege_ == PrivilegeMode::Supervisor && (mstatus_ & TVM)))
            throw std::invalid_argument("SFENCE.VMA requires S/M-mode and TVM permission");
    }
    void set_external(bool asserted) { external_ = asserted; }
    void set_software(bool asserted) { software_ = asserted; }
    void set_timer(bool asserted) { timer_ = asserted; }
    bool wake_pending() const { return (pending() & mie_) != 0; }
    bool requested() const { return selected_interrupt().has_value(); }
    bool waiting() const { return waiting_; }
    void wait_for_interrupt() {
        if (privilege_ == PrivilegeMode::User || (privilege_ == PrivilegeMode::Supervisor && (mstatus_ & TW)))
            throw std::invalid_argument("WFI requires S/M-mode and TW permission");
        waiting_ = !wake_pending();
    }
    void resume() { waiting_ = false; }
    std::uint32_t enter(std::uint32_t pc) {
        const auto selected = selected_interrupt();
        if (!selected) throw std::logic_error("interrupt not enabled/pending at this privilege");
        ++entries_;
        return enter_trap(pc, 0x80000000u | *selected, 0);
    }
    std::uint32_t exception(std::uint32_t pc, std::uint32_t cause, std::uint32_t value) {
        if (cause & 0x80000000u) throw std::invalid_argument("synchronous exception has interrupt bit set");
        ++exceptions_;
        return enter_trap(pc, cause, value);
    }
    std::uint32_t mret() {
        if (privilege_ != PrivilegeMode::Machine) throw std::invalid_argument("MRET requires M-mode");
        privilege_ = previous_privilege();
        mstatus_ = (mstatus_ & ~(MIE | MPIE | MPP)) | ((mstatus_ & MPIE) ? MIE : 0u) | MPIE;
        if (privilege_ != PrivilegeMode::Machine) mstatus_ &= ~MPRV;
        ++returns_;
        return mepc_;
    }
    std::uint32_t sret() {
        if (privilege_ == PrivilegeMode::User || (privilege_ == PrivilegeMode::Supervisor && (mstatus_ & TSR)))
            throw std::invalid_argument("SRET requires S/M-mode and TSR permission");
        privilege_ = (mstatus_ & SPP) ? PrivilegeMode::Supervisor : PrivilegeMode::User;
        mstatus_ = (mstatus_ & ~(SIE | SPIE | SPP | MPRV)) | ((mstatus_ & SPIE) ? SIE : 0u) | SPIE;
        ++returns_;
        return sepc_;
    }
    std::uint32_t read(std::uint16_t csr) const {
        if (csr >= 0x3a0 && csr < 0x3a4) return pmp_.config(csr - 0x3a0);
        if (csr >= 0x3b0 && csr < 0x3c0) return pmp_.address(csr - 0x3b0);
        switch (csr) {
        case 0x100: return mstatus_ & SupervisorStatus;
        case 0x104: return mie_ & mideleg_;
        case 0x105: return stvec_;
        case 0x106: case 0x10a: case 0x306: return 0; // Counter access disabled; no environment extensions.
        case 0x180: return satp_;
        case 0x140: return sscratch_;
        case 0x141: return sepc_;
        case 0x142: return scause_;
        case 0x143: return stval_;
        case 0x144: return pending() & mideleg_;
        case 0x300: return mstatus_;
        case 0x301: return 0x40140100u; // RV32 + I + S + U; fixed WARL ISA configuration.
        case 0x302: return medeleg_;
        case 0x303: return mideleg_;
        case 0x304: return mie_;
        case 0x305: return mtvec_;
        case 0x340: return mscratch_;
        case 0x341: return mepc_;
        case 0x342: return mcause_;
        case 0x343: return mtval_;
        case 0x344: return pending();
        case 0xf11: case 0xf12: case 0xf13: case 0xf14: case 0xf15: return 0;
        default: throw std::invalid_argument("unsupported privileged CSR");
        }
    }
    void write(std::uint16_t csr, std::uint32_t value) {
        if (csr >= 0x3a0 && csr < 0x3a4) { pmp_.set_config(csr - 0x3a0, value); return; }
        if (csr >= 0x3b0 && csr < 0x3c0) { pmp_.set_address(csr - 0x3b0, value); return; }
        switch (csr) {
        case 0x100: mstatus_ = (mstatus_ & ~SupervisorStatus) | (value & SupervisorStatus); break;
        case 0x104: mie_ = (mie_ & ~mideleg_) | (value & mideleg_); break;
        case 0x105: stvec_ = vector(value); break;
        case 0x106: case 0x10a: case 0x306: break;
        case 0x180: satp_ = (value & 0x80000000u) ? value : 0u; break;
        case 0x140: sscratch_ = value; break;
        case 0x141: sepc_ = value & ~3u; break;
        case 0x142: scause_ = value; break;
        case 0x143: stval_ = value; break;
        case 0x144:
            supervisor_pending_ = (supervisor_pending_ & ~(mideleg_ & SSIE)) | (value & mideleg_ & SSIE); break;
        case 0x300: {
            const auto mpp = (value & MPP) >> 11;
            mstatus_ = (value & (MIE | MPIE | MPRV | SupervisorStatus | TVM | TW | TSR)) |
                (mpp == 2 ? 0u : mpp << 11); break;
        }
        case 0x301: break;
        case 0x302: medeleg_ = value & DelegableExceptions; break;
        case 0x303: mideleg_ = value & SupervisorEnables; break;
        case 0x304: mie_ = value & AllEnables; break;
        case 0x305: mtvec_ = vector(value); break;
        case 0x340: mscratch_ = value; break;
        case 0x341: mepc_ = value & ~3u; break;
        case 0x342: mcause_ = value; break;
        case 0x343: mtval_ = value; break;
        case 0x344: supervisor_pending_ = value & SupervisorEnables; break; // Machine inputs remain hardware-driven.
        default: throw std::invalid_argument("unsupported privileged CSR");
        }
    }
    std::uint64_t entries() const { return entries_; }
    std::uint64_t exceptions() const { return exceptions_; }
    std::uint64_t returns() const { return returns_; }
private:
    static std::uint32_t vector(std::uint32_t value) { return (value & ~3u) | ((value & 3u) == 1 ? 1u : 0u); }
    std::uint32_t pending() const {
        return supervisor_pending_ | (external_ ? MEIE : 0u) | (software_ ? MSIE : 0u) | (timer_ ? MTIE : 0u);
    }
    std::optional<unsigned> selected_interrupt() const {
        const auto enabled = pending() & mie_;
        auto eligible = (privilege_ != PrivilegeMode::Machine || (mstatus_ & MIE)) ? enabled & ~mideleg_ : 0u;
        if (privilege_ == PrivilegeMode::User || (privilege_ == PrivilegeMode::Supervisor && (mstatus_ & SIE)))
            eligible |= enabled & mideleg_;
        // M-target interrupts precede S-target interrupts; each bank uses external > software > timer.
        for (unsigned cause : {11u, 3u, 7u, 9u, 1u, 5u})
            if ((eligible & ~mideleg_) & (1u << cause)) return cause;
        for (unsigned cause : {9u, 1u, 5u})
            if ((eligible & mideleg_) & (1u << cause)) return cause;
        return std::nullopt;
    }
    PrivilegeMode previous_privilege() const { return static_cast<PrivilegeMode>((mstatus_ & MPP) >> 11); }
    std::uint32_t enter_trap(std::uint32_t pc, std::uint32_t cause, std::uint32_t value) {
        waiting_ = false;
        const auto number = cause & 0x7fffffffu;
        const auto deleg = (cause & 0x80000000u) ? mideleg_ : medeleg_;
        const bool supervisor = privilege_ != PrivilegeMode::Machine && number < 32 && (deleg & (1u << number));
        std::uint32_t target;
        if (supervisor) {
            sepc_ = pc & ~3u; scause_ = cause; stval_ = value;
            mstatus_ = (mstatus_ & ~(SIE | SPIE | SPP)) | ((mstatus_ & SIE) ? SPIE : 0u) |
                (privilege_ == PrivilegeMode::Supervisor ? SPP : 0u);
            privilege_ = PrivilegeMode::Supervisor; target = stvec_;
        } else {
            mepc_ = pc & ~3u; mcause_ = cause; mtval_ = value;
            mstatus_ = (mstatus_ & ~(MIE | MPIE | MPP)) | ((mstatus_ & MIE) ? MPIE : 0u) |
                (static_cast<std::uint32_t>(privilege_) << 11);
            privilege_ = PrivilegeMode::Machine; target = mtvec_;
        }
        return (target & ~3u) + ((cause & 0x80000000u) && (target & 3u) == 1 ? 4u * number : 0u);
    }
    std::uint32_t mstatus_ = 3u << 11, mie_ = 0, mtvec_ = 0, mepc_ = 0, mcause_ = 0, mtval_ = 0, mscratch_ = 0;
    std::uint32_t medeleg_ = 0, mideleg_ = 0, stvec_ = 0, sepc_ = 0, scause_ = 0, stval_ = 0, sscratch_ = 0;
    std::uint32_t supervisor_pending_ = 0;
    std::uint32_t satp_ = 0;
    bool external_ = false, software_ = false, timer_ = false, waiting_ = false;
    PrivilegeMode privilege_ = PrivilegeMode::Machine;
    PhysicalMemoryProtection pmp_;
    std::uint64_t entries_ = 0, exceptions_ = 0, returns_ = 0;
};
}
