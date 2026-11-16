#pragma once

#include "memory_access.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace colab::soc::core::datapath::dedicated {

inline std::size_t word_index(std::uint32_t address, std::size_t words) {
    if (address % 4 != 0) throw std::invalid_argument("unaligned 32-bit memory address");
    const auto index = static_cast<std::size_t>(address / 4);
    if (index >= words) throw std::out_of_range("address outside memory");
    return index;
}

class InstructionMemory {
public:
    explicit InstructionMemory(std::vector<std::uint32_t> words) : words_(std::move(words)) {
        if (words_.size() > 0x40000000ull) throw std::length_error("instruction memory exceeds 32-bit addresses");
    }
    std::uint32_t read(std::uint32_t address) const { return words_.at(word_index(address, words_.size())); }
private:
    std::vector<std::uint32_t> words_;
};

class DataMemory {
public:
    explicit DataMemory(std::size_t words = 256) : words_(checked_size(words), 0) {}
    std::uint32_t read(std::uint32_t address, unsigned bytes = 4) const {
        validate(address, bytes);
        return (words_[address / 4] >> ((address % 4) * 8)) & memory_mask(bytes);
    }
    void validate(std::uint32_t address, unsigned bytes = 4) const {
        (void)memory_mask(bytes);
        if (address % bytes) throw std::invalid_argument("unaligned scalar memory address");
        if (std::uint64_t(address) + bytes > std::uint64_t(words_.size()) * 4)
            throw std::out_of_range("address outside memory");
    }
    void write(std::uint32_t address, std::uint32_t value, unsigned bytes = 4) {
        validate(address, bytes);
        const auto shift = (address % 4) * 8;
        const auto mask = memory_mask(bytes) << shift;
        auto& word = words_[address / 4];
        word = (word & ~mask) | ((value << shift) & mask);
    }
private:
    static std::size_t checked_size(std::size_t words) {
        if (words > 0x40000000ull) throw std::length_error("data memory exceeds 32-bit addresses");
        return words;
    }
    std::vector<std::uint32_t> words_;
};
}
