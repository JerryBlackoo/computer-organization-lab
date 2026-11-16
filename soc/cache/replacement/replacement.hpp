#pragma once

#include "lru.hpp"
#include "../types.hpp"

#include <limits>

namespace colab::soc::cache {

class Replacement {
public:
    Replacement(std::size_t ways, ReplacementPolicy policy, std::uint32_t seed)
        : policy_(policy), recent_(ways), arrival_(ways), frequency_(ways, 0), random_(seed ? seed : 1) {}

    void inserted(std::size_t way) {
        recent_.touch(way);
        arrival_.touch(way); // FIFO 只在装入时更新，命中不会改变装入先后。
        frequency_.at(way) = 0;
    }

    void accessed(std::size_t way) {
        recent_.touch(way);
        auto& count = frequency_.at(way);
        if (count != std::numeric_limits<std::uint64_t>::max()) ++count;
    }

    std::size_t victim() {
        switch (policy_) {
        case ReplacementPolicy::LRU: return recent_.victim();
        case ReplacementPolicy::FIFO: return arrival_.victim();
        case ReplacementPolicy::Random:
            // 可复现的 xorshift32；种子只用于仿真，不用于安全随机数。
            random_ ^= random_ << 13;
            random_ ^= random_ >> 17;
            random_ ^= random_ << 5;
            return random_ % frequency_.size();
        case ReplacementPolicy::LFU: {
            auto candidate = recent_.victim();
            for (std::size_t way = 0; way < frequency_.size(); ++way) {
                if (frequency_[way] < frequency_[candidate]
                    || (frequency_[way] == frequency_[candidate]
                        && recent_.rank(way) > recent_.rank(candidate))) candidate = way;
            }
            return candidate;
        }
        }
        throw std::logic_error("unknown cache replacement policy");
    }

    std::size_t lru_rank(std::size_t way) const { return recent_.rank(way); }
    std::uint64_t frequency(std::size_t way) const { return frequency_.at(way); }

private:
    ReplacementPolicy policy_;
    LRUReplacement recent_;
    LRUReplacement arrival_;
    std::vector<std::uint64_t> frequency_;
    std::uint32_t random_;
};

} // namespace colab::soc::cache
