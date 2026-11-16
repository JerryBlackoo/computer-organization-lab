#pragma once

#include "../types.hpp"

namespace colab::soc::core::mmu {

// 全相联、固定 4 KiB 页的 TLB 表项。替换状态由 TLB 单独维护。
struct TLBEntry {
    bool valid = false;              // V：槽是否可参与命中判断；不代表页面是否驻留。
    Address virtual_page = 0;        // Tag：完整的 20 位虚页号。
    Address physical_frame = 0;      // PFN：20 位物理页框号。
    Permissions permissions{};      // R/W/X/U：命中后仍按本次请求检查。
    bool accessed = false;           // A：该页已经发生获准的翻译请求。
    bool dirty = false;              // D：该页已被允许写访问，不是 Cache 行的脏位。

    bool matches(Address vpn) const { return valid && virtual_page == vpn; }

    void fill(Address vpn, const PageTableEntry& pte) {
        if (vpn > max_page_number || !pte.present || pte.physical_frame > max_page_number) {
            throw std::invalid_argument("cannot fill TLB with an invalid page translation");
        }
        valid = false;
        virtual_page = vpn;
        physical_frame = pte.physical_frame;
        permissions = pte.permissions;
        accessed = pte.accessed;
        dirty = pte.dirty;
        software_data_ = pte.software_data;
        valid = true; // 地址与控制字段就绪后才让该槽有效。
    }

    void invalidate() { valid = false; } // 保留旧数据也不能再命中。

    PageTableEntry page_table_entry() const {
        if (!valid) throw std::logic_error("cannot use an invalid TLB entry");
        // 只缓存装入时 present=1 的映射。清 V 不等于把主存 PTE 的 P 清零。
        return {physical_frame, true, permissions, accessed, dirty, software_data_};
    }

private:
    // 教学接口按完整 PTE 回写 A/D 时保留的原软件字段，不参与硬件匹配/权限判断。
    std::uint32_t software_data_ = 0;
};

} // namespace colab::soc::core::mmu
