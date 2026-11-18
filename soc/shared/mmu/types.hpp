#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <stdexcept>

namespace colab::soc::core::mmu {

using Address = std::uint32_t;
inline constexpr unsigned page_offset_bits = 12;
inline constexpr Address page_size = Address{1} << page_offset_bits;
inline constexpr Address max_page_number = 0xFFFFF;

enum class AccessType { Read, Write, Execute };
enum class Privilege { User, Supervisor };

// 教学模型独立控制 R/W/X；Supervisor 也必须遵守 R/W/X。
struct Permissions {
    bool readable = false;
    bool writable = false;
    bool executable = false;
    bool user_accessible = false;
};

// 主存中的页表项。present 表示页面驻留主存，与 TLB 槽的 valid 不同。
struct PageTableEntry {
    Address physical_frame = 0;
    bool present = false;
    Permissions permissions{};
    bool accessed = false;
    bool dirty = false; // 页被允许写访问过；与 Cache 行的 dirty 是不同层次的状态。
    std::uint32_t software_data = 0; // 硬件保留、不解释，可供上层保存后备存储标识。
};

enum class FaultReason {
    NotPresent, ReadDenied, WriteDenied, ExecuteDenied, PrivilegeViolation,
    InvalidPageTableEntry, PageTableAccess
};

enum class TranslationStage { Bypass, TLBLookup, PageTableRead, PermissionCheck, AccessBitsUpdate, Complete };

class TranslationFault : public std::runtime_error {
public:
    TranslationFault(Address address, AccessType access, FaultReason reason)
        : std::runtime_error(message(reason)), address_(address), access_(access), reason_(reason) {}

    Address address() const { return address_; }
    AccessType access() const { return access_; }
    FaultReason reason() const { return reason_; }

private:
    static const char* message(FaultReason reason) {
        switch (reason) {
        case FaultReason::NotPresent: return "page is not present in physical memory";
        case FaultReason::ReadDenied: return "page read permission denied";
        case FaultReason::WriteDenied: return "page write permission denied";
        case FaultReason::ExecuteDenied: return "page execute permission denied";
        case FaultReason::PrivilegeViolation: return "user access to supervisor page denied";
        case FaultReason::InvalidPageTableEntry: return "invalid physical frame or reserved page table bits";
        case FaultReason::PageTableAccess: return "physical page table access failed";
        }
        return "address translation fault";
    }

    Address address_;
    AccessType access_;
    FaultReason reason_;
};

struct TranslationResult {
    Address physical_address;
    Address virtual_page;
    Address physical_frame;
    Address page_offset;
    bool paging_enabled;
    bool tlb_hit;
};

struct TranslatedSpan {
    Address virtual_address;
    Address physical_address;
    std::size_t size;
};

struct TranslationTrace {
    Address virtual_address = 0;
    AccessType access = AccessType::Read;
    Privilege privilege = Privilege::User;
    TranslationStage stage = TranslationStage::Bypass;
    bool tlb_hit = false;
    bool page_walk = false;
    bool accessed_updated = false;
    bool dirty_updated = false;
    std::optional<Address> page_table_entry_address;
    std::optional<Address> physical_address;
    std::optional<FaultReason> fault;
};

struct FaultInfo {
    Address virtual_address;
    AccessType access;
    Privilege privilege;
    FaultReason reason;
    TranslationStage stage;
    Address page_table_base;
    bool tlb_hit;
    std::optional<Address> page_table_entry_address;
};

struct Statistics {
    std::uint64_t tlb_hits = 0;
    std::uint64_t tlb_misses = 0;
    std::uint64_t page_walks = 0;
    std::uint64_t page_faults = 0;
    std::uint64_t protection_faults = 0;
    std::uint64_t invalid_entries = 0;
    std::uint64_t page_table_faults = 0;
    std::uint64_t access_bit_writes = 0;
};

} // namespace colab::soc::core::mmu
