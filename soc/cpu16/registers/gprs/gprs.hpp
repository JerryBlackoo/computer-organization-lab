#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace colab::soc::core::registers {
class GPRs {
public:
    static constexpr std::size_t count = 16;
    std::uint16_t read(std::size_t index) const { return values_.at(index); }
    void write(std::size_t index, std::uint16_t value) { values_.at(index) = value; }
private:
    std::array<std::uint16_t, count> values_{};
};
}
