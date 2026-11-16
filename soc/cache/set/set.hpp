#pragma once

#include "../line/line.hpp"
#include "../replacement/replacement.hpp"

#include <optional>

namespace colab::soc::cache {

class Set {
public:
    Set(std::size_t ways, std::size_t line_bytes,
        ReplacementPolicy policy = ReplacementPolicy::LRU, std::uint32_t seed = 1)
        : replacement_(ways, policy, seed) {
        lines_.reserve(ways);
        for (std::size_t i = 0; i < ways; ++i) lines_.emplace_back(line_bytes);
    }

    // 对应硬件中各路 Tag 比较器 + valid 判断；返回值用于选择命中的数据路。
    std::optional<std::size_t> find(Address tag) const {
        for (std::size_t way = 0; way < lines_.size(); ++way) {
            if (lines_[way].valid && lines_[way].tag == tag) return way;
        }
        return std::nullopt;
    }

    std::size_t victim() {
        for (std::size_t way = 0; way < lines_.size(); ++way) {
            if (!lines_[way].valid) return way;
        }
        return replacement_.victim();
    }

    void inserted(std::size_t way) { replacement_.inserted(way); }
    void touch(std::size_t way) { replacement_.accessed(way); }
    std::size_t lru_rank(std::size_t way) const { return replacement_.lru_rank(way); }
    std::uint64_t frequency(std::size_t way) const { return replacement_.frequency(way); }
    Line& line(std::size_t way) { return lines_.at(way); }
    const Line& line(std::size_t way) const { return lines_.at(way); }

private:
    std::vector<Line> lines_;
    Replacement replacement_;
};

} // namespace colab::soc::cache
