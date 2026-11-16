#pragma once

#include "../interrupt_system/interrupt_system.hpp"
#include "../../../devices/common/dma_bus.hpp"
#include <array>
#include <optional>

namespace colab::soc::core {
// Sv32 + Svade: A=0 or a store with D=0 faults for guest software to repair.
// Page-table reads are real aligned 32-bit RAM transactions, with S-mode PMP.
// One walk is shared by instruction/data access; redirects drain abandoned reads.
class Sv32 {
public:
    struct Statistics {
        std::uint64_t hits = 0, misses = 0, walks = 0, reads = 0, faults = 0, flushes = 0;
        std::uint64_t page_faults = 0, access_faults = 0;
    };
    struct Entry {
        bool valid = false, global = false;
        unsigned level = 0, asid = 0;
        std::uint32_t vpn = 0, pte = 0;
    };
    const Statistics& statistics() const { return stats_; }
    const std::array<Entry, 16>& entries() const { return entries_; }
    bool busy() const { return walk_.has_value() || pending_.has_value(); }
    void cancel() { walk_.reset(); if (pending_) discarded_ = true; }
    void flush() {
        cancel(); for (auto& entry : entries_) entry.valid = false; ++stats_.flushes;
    }
    void drain() {
        if (discarded_ && pending_ && pending_bus_->ready(*pending_)) {
            pending_bus_->take(*pending_); pending_.reset(); discarded_ = false;
        }
    }
    std::optional<std::uint32_t> translate(std::uint32_t address, unsigned bytes, mmu::AccessType access,
                                         std::uint32_t pc, const InterruptSystem& state,
                                         devices::common::DmaBus* bus,
                                         const datapath::dedicated::DataMemory& memory) {
        const Request request{address, pc, state.read(0x180), state.read(0x300), bytes, access,
            access == mmu::AccessType::Execute ? state.privilege() : state.data_privilege()};
        if (walk_ && !(walk_->request == request)) cancel();
        drain();
        if (discarded_) return std::nullopt;
        try {
            if (!state.sv32_enabled(access)) { state.check_memory(address, bytes, access, pc); return address; }
            if (!walk_) {
                const auto asid = (request.satp >> 22) & 511u;
                for (const auto& entry : entries_) if (entry.valid && (entry.global || entry.asid == asid) &&
                    (entry.level ? entry.vpn >> 10 == address >> 22 : entry.vpn == address >> 12)) {
                    ++stats_.hits;
                    return leaf(entry.pte, entry.level, request, state);
                }
                ++stats_.misses; ++stats_.walks;
                walk_ = Walk{request, std::uint64_t(request.satp & 0x3fffffu) << 12, 1, false};
            }
            while (walk_) {
                auto& walk = *walk_;
                const auto index = (address >> (12 + 10 * walk.level)) & 1023u;
                const auto pte_address = walk.table + 4 * index;
                if (pte_address > UINT32_MAX || !state.page_table_read_allowed(static_cast<std::uint32_t>(pte_address)))
                    access_fault(request);
                std::uint32_t pte;
                if (bus) {
                    if (!bus->is_memory(static_cast<std::uint32_t>(pte_address), 4)) access_fault(request);
                    if (!pending_) {
                        try { pending_ = bus->submit(0, devices::common::BusOperation::Read, pte_address, 4); }
                        catch (const std::length_error&) { return std::nullopt; }
                        catch (const std::exception&) { access_fault(request); }
                        pending_bus_ = bus; ++stats_.reads;
                    }
                    if (!bus->ready(*pending_)) return std::nullopt;
                    const auto result = bus->take(*pending_); pending_.reset();
                    if (result.status != interconnect::pcie::Status::Success || result.transferred_bytes != 4 || result.data.size() != 4)
                        access_fault(request);
                    pte = devices::common::load32(result.data);
                } else {
                    try { pte = memory.read(static_cast<std::uint32_t>(pte_address)); }
                    catch (const std::out_of_range&) { access_fault(request); }
                    ++stats_.reads;
                }
                if (!(pte & 1) || (!(pte & 2) && (pte & 4))) page_fault(request);
                walk.global = walk.global || (pte & 32);
                if (pte & 10) {
                    const auto physical = leaf(pte, walk.level, request, state);
                    entries_[victim_++ % entries_.size()] = {true, walk.global, walk.level,
                        (request.satp >> 22) & 511u, address >> 12, pte};
                    walk_.reset(); return physical;
                }
                if (!walk.level || (pte & 0xd0)) page_fault(request); // Non-leaf U/A/D are reserved.
                walk.table = std::uint64_t(pte >> 10) << 12; --walk.level;
            }
        } catch (const RV32Fault& error) {
            ++stats_.faults;
            if (error.cause() == RV32FaultCause::InstructionPageFault || error.cause() == RV32FaultCause::LoadPageFault ||
                error.cause() == RV32FaultCause::StorePageFault) ++stats_.page_faults;
            else ++stats_.access_faults;
            cancel(); throw;
        }
        throw std::logic_error("Sv32 walk has no result");
    }
private:
    struct Request {
        std::uint32_t address, pc, satp, status;
        unsigned bytes;
        mmu::AccessType access;
        PrivilegeMode privilege;
        bool operator==(const Request& other) const {
            return address == other.address && pc == other.pc && satp == other.satp && status == other.status &&
                bytes == other.bytes && access == other.access && privilege == other.privilege;
        }
    };
    struct Walk { Request request; std::uint64_t table; unsigned level; bool global; };
    [[noreturn]] static void page_fault(const Request& request) {
        throw RV32Fault(request.access == mmu::AccessType::Execute ? RV32FaultCause::InstructionPageFault :
            request.access == mmu::AccessType::Read ? RV32FaultCause::LoadPageFault : RV32FaultCause::StorePageFault,
            request.pc, request.address);
    }
    [[noreturn]] static void access_fault(const Request& request) {
        throw RV32Fault(request.access == mmu::AccessType::Execute ? RV32FaultCause::InstructionAccessFault :
            request.access == mmu::AccessType::Read ? RV32FaultCause::LoadAccessFault : RV32FaultCause::StoreAccessFault,
            request.pc, request.address);
    }
    static std::uint32_t leaf(std::uint32_t pte, unsigned level, const Request& request, const InterruptSystem& state) {
        if (level && ((pte >> 10) & 1023)) page_fault(request);
        const bool user = pte & 16;
        if (request.privilege == PrivilegeMode::User ? !user :
            user && (request.access == mmu::AccessType::Execute || !(request.status & InterruptSystem::SUM))) page_fault(request);
        const bool permission = request.access == mmu::AccessType::Execute ? pte & 8 :
            request.access == mmu::AccessType::Write ? pte & 4 : (pte & 2) || ((request.status & InterruptSystem::MXR) && (pte & 8));
        if (!permission || !(pte & 64) || (request.access == mmu::AccessType::Write && !(pte & 128))) page_fault(request);
        const auto physical = level ? (std::uint64_t(pte >> 20) << 22) | (request.address & 0x3fffffu) :
            (std::uint64_t(pte >> 10) << 12) | (request.address & 0xfffu);
        if (physical > UINT32_MAX) access_fault(request); // Never truncate Sv32's high PPN bits.
        state.check_memory(static_cast<std::uint32_t>(physical), request.bytes, request.access, request.pc, request.address);
        return static_cast<std::uint32_t>(physical);
    }
    std::optional<Walk> walk_;
    std::optional<devices::common::Tag> pending_;
    devices::common::DmaBus* pending_bus_ = nullptr;
    bool discarded_ = false;
    std::array<Entry, 16> entries_{};
    std::size_t victim_ = 0;
    Statistics stats_;
};
} // namespace colab::soc::core
