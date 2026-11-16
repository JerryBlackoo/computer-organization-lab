#pragma once

#include "entry.hpp"

#include <cstddef>
#include <numeric>
#include <optional>
#include <vector>

namespace colab::soc::core::mmu {

class TLB {
public:
    explicit TLB(std::size_t capacity = 16) : entries_(capacity), order_(capacity) {
        if (capacity == 0) throw std::invalid_argument("TLB requires at least one entry");
        std::iota(order_.begin(), order_.end(), std::size_t{0});
    }

    std::optional<PageTableEntry> lookup(Address virtual_page) {
        check_page(virtual_page);
        for (std::size_t i = 0; i < entries_.size(); ++i) {
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
        auto victim = order_.front();
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i].matches(virtual_page)) {
                victim = i;
                entries_[victim].fill(virtual_page, entry);
                touch(victim);
                return;
            }
        }
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            if (!entries_[i].valid) { victim = i; break; }
        }
        entries_[victim].fill(virtual_page, entry);
        touch(victim);
    }

    void invalidate(Address virtual_page) {
        check_page(virtual_page);
        for (auto& entry : entries_) {
            if (entry.matches(virtual_page)) entry.invalidate();
        }
    }

    void flush() { for (auto& entry : entries_) entry.invalidate(); }
    const std::vector<TLBEntry>& entries() const { return entries_; }
    std::size_t lru_rank(std::size_t slot) const {
        if (slot >= entries_.size()) throw std::out_of_range("TLB slot out of range");
        for (std::size_t i = 0; i < order_.size(); ++i) {
            if (order_[i] == slot) return order_.size() - 1 - i; // 0 为 MRU。
        }
        throw std::logic_error("invalid TLB replacement state");
    }

private:
    static void check_page(Address virtual_page) {
        if (virtual_page > max_page_number) throw std::out_of_range("virtual page exceeds 20 bits");
    }
    // order_ 从 LRU 到 MRU 排列，无时间戳溢出问题。
    void touch(std::size_t index) {
        std::size_t position = 0;
        while (order_[position] != index) ++position;
        for (; position + 1 < order_.size(); ++position) order_[position] = order_[position + 1];
        order_.back() = index;
    }

    std::vector<TLBEntry> entries_;
    std::vector<std::size_t> order_;
};

} // namespace colab::soc::core::mmu
