#pragma once

#include <cstddef>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace colab::soc::cache {

class LRUReplacement {
public:
    explicit LRUReplacement(std::size_t ways) : order_(ways) {
        if (ways == 0) throw std::invalid_argument("LRU requires at least one way");
        std::iota(order_.begin(), order_.end(), std::size_t{0});
    }

    std::size_t victim() const { return order_.front(); }
    std::size_t rank(std::size_t way) const {
        if (way >= order_.size()) throw std::out_of_range("cache way out of range");
        for (std::size_t i = 0; i < order_.size(); ++i) {
            if (order_[i] == way) return order_.size() - 1 - i; // 0 表示 MRU。
        }
        throw std::logic_error("invalid LRU order");
    }
    void touch(std::size_t way) {
        if (way >= order_.size()) throw std::out_of_range("cache way out of range");
        std::size_t position = 0;
        while (order_[position] != way) ++position;
        for (; position + 1 < order_.size(); ++position) order_[position] = order_[position + 1];
        order_.back() = way;
    }

private:
    std::vector<std::size_t> order_; // 从 LRU 到 MRU。
};

} // namespace colab::soc::cache
