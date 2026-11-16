#pragma once

#include "../privilege.hpp"
#include "../mmu/types.hpp"
#include <array>
#include <utility>

namespace colab::soc::core {
// RV32 base PMP: 16 entries, 4-byte granularity, 32-bit physical addresses.
// No Smepmp. Unlocked entries constrain S/U; locked entries also constrain M.
class PhysicalMemoryProtection {
public:
    static constexpr unsigned EntryCount = 16;
    static constexpr std::uint8_t R = 1, W = 2, X = 4, Tor = 8, Na4 = 16, Napot = 24, Lock = 128;
    std::uint32_t config(unsigned word) const {
        if (word >= 4) throw std::out_of_range("PMP config CSR index");
        std::uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= std::uint32_t(config_[word * 4 + i]) << (i * 8);
        return value;
    }
    void set_config(unsigned word, std::uint32_t value) {
        if (word >= 4) throw std::out_of_range("PMP config CSR index");
        for (unsigned i = 0; i < 4; ++i) {
            auto& entry = config_[word * 4 + i];
            if (entry & Lock) continue;
            auto next = static_cast<std::uint8_t>((value >> (i * 8)) & 0x9fu);
            if (!(next & R)) next &= ~W; // WARL: reserved R=0,W=1 becomes W=0.
            entry = next;
        }
    }
    std::uint32_t address(unsigned entry) const { return address_.at(entry); }
    void set_address(unsigned entry, std::uint32_t value) {
        auto& target = address_.at(entry);
        if ((config_[entry] & Lock) || (entry + 1 < EntryCount &&
            (config_[entry + 1] & (Lock | Napot)) == (Lock | Tor))) return;
        target = value & 0x3fffffffu;
    }
    bool allows(std::uint32_t address, std::size_t bytes, mmu::AccessType access, PrivilegeMode privilege) const {
        if (!bytes || bytes > (std::uint64_t{1} << 32) - address) return false;
        const auto end = std::uint64_t(address) + bytes;
        for (unsigned i = 0; i < EntryCount; ++i) {
            const auto [lower, upper] = region(i);
            if (lower >= upper || end <= lower || std::uint64_t(address) >= upper) continue;
            // First any-byte match must contain the entire access, even in M.
            if (address < lower || end > upper) return false;
            if (privilege == PrivilegeMode::Machine && !(config_[i] & Lock)) return true;
            const auto bit = access == mmu::AccessType::Read ? R : access == mmu::AccessType::Write ? W : X;
            return (config_[i] & bit) != 0;
        }
        return privilege == PrivilegeMode::Machine;
    }
private:
    std::pair<std::uint64_t, std::uint64_t> region(unsigned entry) const {
        const auto mode = config_[entry] & Napot;
        const auto address = std::uint64_t(address_[entry]);
        if (mode == Tor) return {entry ? std::uint64_t(address_[entry - 1]) << 2 : 0, address << 2};
        if (mode == Na4) return {address << 2, (address << 2) + 4};
        if (mode == Napot) {
            unsigned ones = 0;
            while (ones < 30 && (address & (std::uint64_t{1} << ones))) ++ones;
            const auto lower = (address & ~((std::uint64_t{1} << ones) - 1)) << 2;
            return {lower, lower + (std::uint64_t{1} << (ones + 3))};
        }
        return {0, 0};
    }
    std::array<std::uint8_t, EntryCount> config_{};
    std::array<std::uint32_t, EntryCount> address_{};
};
} // namespace colab::soc::core
