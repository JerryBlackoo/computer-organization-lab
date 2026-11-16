#pragma once

#include "address_translator/address_translator.hpp"
#include "fault_registers/fault_registers.hpp"
#include "page_table_walker/page_table_walker.hpp"
#include "permission_checker/permission_checker.hpp"
#include "tlb/tlb.hpp"

#include <utility>
#include <algorithm>
#include <limits>
#include <vector>

namespace colab::soc::core {

// MMU 只输出获准访问的物理地址；实际数据由独立的 Cache/主存对象提供。
class MMU {
public:
    explicit MMU(std::size_t tlb_entries = 16) : tlb_(tlb_entries) {}

    // 默认关闭分页，兼容现有 16 位 CPU。切换页表时清除旧地址空间的快表。
    void enable_paging(mmu::Address page_table_base, mmu::PageTableWalker::ReadEntry reader,
                       mmu::PageTableWalker::WriteEntry writer = {}) {
        walker_.configure(page_table_base, std::move(reader), std::move(writer));
        tlb_.flush();
        enabled_ = true;
    }

    void disable_paging() { enabled_ = false; tlb_.flush(); }
    bool paging_enabled() const { return enabled_; }
    bool access_bits_writeback_enabled() const { return walker_.can_write(); }
    void invalidate_page(mmu::Address virtual_page) { tlb_.invalidate(virtual_page); }
    void flush_tlb() { tlb_.flush(); }

    mmu::Address translate(mmu::Address virtual_address,
                           mmu::AccessType access = mmu::AccessType::Read,
                           mmu::Privilege privilege = mmu::Privilege::User) {
        return translate_with_info(virtual_address, access, privilege).physical_address;
    }

    mmu::TranslationResult translate_with_info(
        mmu::Address virtual_address, mmu::AccessType access = mmu::AccessType::Read,
        mmu::Privilege privilege = mmu::Privilege::User) {
        const auto parts = mmu::AddressTranslator::split(virtual_address);
        mmu::TranslationTrace trace;
        trace.virtual_address = virtual_address;
        trace.access = access;
        trace.privilege = privilege;
        if (!enabled_) {
            trace.physical_address = virtual_address;
            last_translation_ = trace;
            return {virtual_address, parts.virtual_page, parts.virtual_page,
                    parts.page_offset, false, false};
        }

        try {
            // TLB 未命中只触发遍历；present=0 才是缺页。
            trace.stage = mmu::TranslationStage::TLBLookup;
            const auto cached = tlb_.lookup(parts.virtual_page);
            trace.tlb_hit = cached.has_value();
            if (cached) ++statistics_.tlb_hits;
            else ++statistics_.tlb_misses;

            mmu::PageTableEntry entry;
            if (cached) entry = *cached;
            else {
                trace.stage = mmu::TranslationStage::PageTableRead;
                trace.page_walk = true;
                ++statistics_.page_walks;
                try {
                    trace.page_table_entry_address = walker_.entry_address(parts.virtual_page);
                    entry = walker_.walk(parts.virtual_page);
                } catch (const mmu::PageTableFormatError&) {
                    throw mmu::TranslationFault(virtual_address, access, mmu::FaultReason::InvalidPageTableEntry);
                } catch (const std::exception&) {
                    throw mmu::TranslationFault(virtual_address, access, mmu::FaultReason::PageTableAccess);
                }
            }

            // 命中也必须重新按本次访问类型、特权级检查。
            trace.stage = mmu::TranslationStage::PermissionCheck;
            mmu::PermissionChecker::check(entry, virtual_address, access, privilege);
            trace.stage = mmu::TranslationStage::AccessBitsUpdate;
            const bool set_accessed = !entry.accessed;
            const bool set_dirty = access == mmu::AccessType::Write && !entry.dirty;
            if (set_accessed || set_dirty) {
                entry.accessed = true;
                entry.dirty = entry.dirty || access == mmu::AccessType::Write;
                if (walker_.can_write()) {
                    try {
                        trace.page_table_entry_address = walker_.entry_address(parts.virtual_page);
                        walker_.write(parts.virtual_page, entry);
                    } catch (const std::exception&) {
                        throw mmu::TranslationFault(virtual_address, access, mmu::FaultReason::PageTableAccess);
                    }
                    ++statistics_.access_bit_writes;
                }
                trace.accessed_updated = set_accessed;
                trace.dirty_updated = set_dirty;
            }
            // 只有检查与必要的页表回写都成功后，才填充/更新快表和输出物理地址。
            if (!cached || set_accessed || set_dirty) tlb_.insert(parts.virtual_page, entry);
            const auto physical_address = mmu::AddressTranslator::compose(entry.physical_frame, parts.page_offset);
            trace.stage = mmu::TranslationStage::Complete;
            trace.physical_address = physical_address;
            last_translation_ = trace;
            return {physical_address, parts.virtual_page, entry.physical_frame,
                    parts.page_offset, true, cached.has_value()};
        } catch (const mmu::TranslationFault& fault) {
            switch (fault.reason()) {
            case mmu::FaultReason::NotPresent: ++statistics_.page_faults; break;
            case mmu::FaultReason::InvalidPageTableEntry: ++statistics_.invalid_entries; break;
            case mmu::FaultReason::PageTableAccess: ++statistics_.page_table_faults; break;
            default: ++statistics_.protection_faults; break;
            }
            trace.fault = fault.reason();
            fault_registers_.latch(trace, fault.reason(), walker_.page_table_base());
            last_translation_ = trace;
            throw;
        }
    }

    // 先逐页检查整个虚拟范围，供外层在全部成功后发出数据请求。
    // 每段独立翻译，不能假定相邻虚页对应相邻物理页。
    std::vector<mmu::TranslatedSpan> translate_range(
        mmu::Address address, std::size_t size, mmu::AccessType access = mmu::AccessType::Read,
        mmu::Privilege privilege = mmu::Privilege::User) {
        if (size > std::uint64_t{std::numeric_limits<mmu::Address>::max()} - address + 1) {
            throw std::out_of_range("virtual range exceeds 32-bit address space");
        }
        std::vector<mmu::TranslatedSpan> spans;
        for (std::size_t done = 0; done < size;) {
            const auto current = static_cast<mmu::Address>(address + done);
            const auto count = std::min(size - done, std::size_t{mmu::page_size - current % mmu::page_size});
            spans.push_back({current, translate(current, access, privilege), count});
            done += count;
        }
        return spans;
    }

    const std::optional<mmu::FaultInfo>& fault() const { return fault_registers_.fault(); }
    void clear_fault() { fault_registers_.clear(); }
    const std::optional<mmu::TranslationTrace>& last_translation() const { return last_translation_; }

    const mmu::TLB& tlb() const { return tlb_; }
    const mmu::PageTableWalker& page_table_walker() const { return walker_; }
    const mmu::Statistics& statistics() const { return statistics_; }

private:
    bool enabled_ = false;
    mmu::TLB tlb_;
    mmu::PageTableWalker walker_;
    mmu::Statistics statistics_;
    mmu::FaultRegisters fault_registers_;
    std::optional<mmu::TranslationTrace> last_translation_;
};

} // namespace colab::soc::core
