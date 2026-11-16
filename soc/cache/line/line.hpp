#pragma once

#include "../types.hpp"

namespace colab::soc::cache {

struct Line {
    bool valid = false;
    bool dirty = false; // 写回策略下，该行是否比下级存储更新。
    Address tag = 0;
    Bytes data;

    explicit Line(std::size_t line_bytes) : data(line_bytes, 0) {}
};

} // namespace colab::soc::cache
