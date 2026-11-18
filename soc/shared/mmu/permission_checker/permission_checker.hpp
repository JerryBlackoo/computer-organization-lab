#pragma once

#include "../types.hpp"

namespace colab::soc::core::mmu {

class PermissionChecker {
public:
    // 快表命中也必须检查；获得物理页框号不代表本次访问被允许。
    static void check(const PageTableEntry& entry, Address address,
                      AccessType access, Privilege privilege) {
        if (!entry.present) throw TranslationFault(address, access, FaultReason::NotPresent);
        if (entry.physical_frame > max_page_number) {
            throw TranslationFault(address, access, FaultReason::InvalidPageTableEntry);
        }
        if (privilege == Privilege::User && !entry.permissions.user_accessible) {
            throw TranslationFault(address, access, FaultReason::PrivilegeViolation);
        }
        if (access == AccessType::Read && !entry.permissions.readable) {
            throw TranslationFault(address, access, FaultReason::ReadDenied);
        }
        if (access == AccessType::Write && !entry.permissions.writable) {
            throw TranslationFault(address, access, FaultReason::WriteDenied);
        }
        if (access == AccessType::Execute && !entry.permissions.executable) {
            throw TranslationFault(address, access, FaultReason::ExecuteDenied);
        }
    }
};

} // namespace colab::soc::core::mmu
