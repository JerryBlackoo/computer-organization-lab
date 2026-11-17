#pragma once

#include "entry.hpp"
#include "geometry.hpp"

#include <cstddef>
#include <numeric>
#include <optional>
#include <vector>

namespace colab::soc::core::mmu {

class TLB {
public:
    explicit TLB(std::size_t capacity = 16) : TLB(TlbConfig{capacity, capacity}) {}
    explicit TLB(TlbConfig config)
        : geometry_(config), entries_(geometry_.capacity()), order_(geometry_.capacity()) {
        std::iota(order_.begin(), order_.end(), std::size_t{0});
    }
    const TlbGeometry& geometry() const { return geometry_; }

    std::optional<PageTableEntry> lookup(Address virtual_page) {
        check_page(virtual_page);
        const auto first = geometry_.first(geometry_.set_index(virtual_page));
        for (std::size_t i = first; i < first + geometry_.ways(); ++i) {
            if (entries_[i].matches(virtual_page)) {
                touch(i);
                return entries_[i].page_table_entry();
            }
        }
        return std::nullopt;
    }

    void insert(Address virtual_page, const PageTableEntry& entry) {
        if (virtual_page > max_page_number || !entry.present || entry.physical_frame > max_page_number) {
            throw std::invalid_argument("cannot cache an invalid page translation");
        }
        const auto first = geometry_.first(geometry_.set_index(virtual_page));
        auto victim = order_[first];
        for (std::size_t i = first; i < first + geometry_.ways(); ++i) {
            if (entries_[i].matches(virtual_page)) {
                victim = i;
                entries_[victim].fill(virtual_page, entry);
                touch(victim);
                return;
            }
        }
        for (std::size_t i = first; i < first + geometry_.ways(); ++i) {
            if (!entries_[i].valid) { victim = i; break; }
        }
        entries_[victim].fill(virtual_page, entry);
        touch(victim);
    }

    void invalidate(Address virtual_page) {
        check_page(virtual_page);
        const auto first = geometry_.first(geometry_.set_index(virtual_page));
        for (std::size_t i = first; i < first + geometry_.ways(); ++i) {
            if (entries_[i].matches(virtual_page)) entries_[i].invalidate();
        }
    }

    void flush() { for (auto& entry : entries_) entry.invalidate(); }
    const std::vector<TLBEntry>& entries() const { return entries_; }
    std::size_t lru_rank(std::size_t slot) const {
        if (slot >= entries_.size()) throw std::out_of_range("TLB slot out of range");
        const auto first = slot / geometry_.ways() * geometry_.ways();
        for (std::size_t i = first; i < first + geometry_.ways(); ++i) {
            if (order_[i] == slot) return first + geometry_.ways() - 1 - i; // 组内 0 为 MRU。
        }
        throw std::logic_error("invalid TLB replacement state");
    }

private:
    static void check_page(Address virtual_page) {
        if (virtual_page > max_page_number) throw std::out_of_range("virtual page exceeds 20 bits");
    }
    // 每组的 order_ 子区间从 LRU 到 MRU 排列，无时间戳溢出问题。
    void touch(std::size_t index) {
        const auto end = (index / geometry_.ways() + 1) * geometry_.ways();
        std::size_t position = end - geometry_.ways();
        while (order_[position] != index) ++position;
        for (; position + 1 < end; ++position) order_[position] = order_[position + 1];
        order_[end - 1] = index;
    }

    TlbGeometry geometry_;
    std::vector<TLBEntry> entries_;
    std::vector<std::size_t> order_;
};

} // namespace colab::soc::core::mmu
