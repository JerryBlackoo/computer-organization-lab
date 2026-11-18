#pragma once

#include "../types.hpp"
#include "../page_table_entry/page_table_entry.hpp"

#include <functional>
#include <limits>
#include <utility>

namespace colab::soc::core::mmu {

class PageTableWalker {
public:
    // 回调参数是页表项的物理字节地址，绝不能再次送入 MMU。
    using ReadEntry = std::function<PageTableEntry(Address)>;
    using WriteEntry = std::function<void(Address, const PageTableEntry&)>;
    // 单级页表的教学布局：每项占 8 字节，回调负责解码成 PageTableEntry。
    static constexpr Address entry_bytes = 8;

    void configure(Address page_table_base, ReadEntry read_entry, WriteEntry write_entry = {}) {
        if (!read_entry || page_table_base % entry_bytes != 0) {
            throw std::invalid_argument("page table requires a reader and an 8-byte aligned base");
        }
        read_entry_ = std::move(read_entry);
        write_entry_ = std::move(write_entry);
        page_table_base_ = page_table_base;
    }

    PageTableEntry walk(Address virtual_page) const {
        if (!read_entry_) throw std::logic_error("page table reader is not configured");
        return read_entry_(entry_address(virtual_page));
    }

    Address entry_address(Address virtual_page) const {
        if (virtual_page > max_page_number) throw std::out_of_range("virtual page exceeds 20 bits");
        const auto address = std::uint64_t{page_table_base_}
            + std::uint64_t{virtual_page} * entry_bytes;
        if (address + entry_bytes - 1 > std::numeric_limits<Address>::max()) {
            throw std::out_of_range("page table entry exceeds physical address space");
        }
        return static_cast<Address>(address);
    }

    bool can_write() const { return static_cast<bool>(write_entry_); }
    void write(Address virtual_page, const PageTableEntry& entry) const {
        if (!write_entry_) throw std::logic_error("page table writer is not configured");
        write_entry_(entry_address(virtual_page), entry);
    }

    Address page_table_base() const { return page_table_base_; }

private:
    Address page_table_base_ = 0;
    ReadEntry read_entry_;
    WriteEntry write_entry_;
};

} // namespace colab::soc::core::mmu
