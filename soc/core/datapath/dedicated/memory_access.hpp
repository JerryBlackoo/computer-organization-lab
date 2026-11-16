#pragma once

#include <cstdint>
#include <stdexcept>

namespace colab::soc::core::datapath::dedicated {
inline std::uint32_t memory_mask(unsigned bytes) {
    switch (bytes) {
    case 1: return 0xffu;
    case 2: return 0xffffu;
    case 4: return 0xffffffffu;
    default: throw std::invalid_argument("unsupported scalar memory width");
    }
}
inline std::uint32_t extend_load(std::uint32_t value, unsigned bytes, bool sign) {
    const auto mask = memory_mask(bytes);
    value &= mask;
    if (sign && bytes < 4 && (value & (1u << (bytes * 8 - 1)))) value |= ~mask;
    return value;
}
} // namespace colab::soc::core::datapath::dedicated
