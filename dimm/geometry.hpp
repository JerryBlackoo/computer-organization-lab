#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace colab::dimm {

// 一个 Word 是整个 Rank 并行提供的数据，最多支持 64 位。
using Word = std::uint64_t;

namespace detail {
inline std::size_t multiply(std::size_t a, std::size_t b) {
    if (b != 0 && a > std::numeric_limits<std::size_t>::max() / b) {
        throw std::overflow_error("DRAM geometry overflows size_t");
    }
    return a * b;
}

inline std::size_t bytes_for_bits(std::size_t bits) {
    return bits / 8 + (bits % 8 != 0 ? 1 : 0);
}

inline Word mask(std::size_t bits) {
    return bits == 64 ? std::numeric_limits<Word>::max() : (Word{1} << bits) - 1;
}

inline void check_value(Word value, std::size_t bits) {
    if ((value & ~mask(bits)) != 0) {
        throw std::out_of_range("data exceeds DRAM data width");
    }
}
} // namespace detail

struct BankGeometry {
    std::size_t rows = 2048;
    std::size_t columns = 2048;
    std::size_t data_bits = 4; // 每个行列交点的位平面数，即芯片的 x4 等 I/O 宽度。

    void validate() const {
        if (rows == 0 || columns == 0 || data_bits == 0 || data_bits > 8) {
            throw std::invalid_argument("bank requires rows/columns > 0 and 1..8 data bits");
        }
        (void)capacity_bits();
    }
    std::size_t word_count() const { return detail::multiply(rows, columns); }
    std::size_t capacity_bits() const { return detail::multiply(word_count(), data_bits); }
    std::size_t capacity_bytes() const { return detail::bytes_for_bits(capacity_bits()); }

    // 行列地址复用同一组引脚，所需引脚数为 max(ceil(log2(rows)), ceil(log2(columns)))。
    // 该值是物理复用引脚数，不是完整线性地址位数：2048 行 x 2048 列需 22 位逻辑地址，
    // 经 RAS/CAS 分两次使用 11 根引脚传入；另有 Bank/Rank 选择，不能据此当成 2K 容量。
    std::size_t address_pin_count() const {
        if (rows == 0 || columns == 0) throw std::invalid_argument("empty bank geometry");
        auto highest_address = (rows > columns ? rows : columns) - 1;
        std::size_t count = 0;
        while (highest_address != 0) { ++count; highest_address >>= 1; }
        return count;
    }
};

struct DRAMChipGeometry {
    std::size_t banks = 1; // 默认一个 Bank，整颗芯片恰好对应图中的 4M x 4 bit。
    BankGeometry bank{};

    void validate() const {
        bank.validate();
        if (banks == 0) throw std::invalid_argument("DRAM chip requires at least one bank");
        (void)capacity_bits();
    }
    std::size_t word_count() const { return detail::multiply(banks, bank.word_count()); }
    std::size_t capacity_bits() const { return detail::multiply(word_count(), bank.data_bits); }
    std::size_t capacity_bytes() const { return detail::bytes_for_bits(capacity_bits()); }
};

struct RankGeometry {
    std::size_t dram_chips = 4; // 四颗 x4 DRAM chip 并行组成 16 位数据字。
    DRAMChipGeometry dram_chip{};

    void validate() const {
        dram_chip.validate();
        if (dram_chips == 0 || dram_chips > 64 / dram_chip.bank.data_bits) {
            throw std::invalid_argument("rank data width must be 1..64 bits");
        }
        (void)capacity_bits();
    }
    std::size_t word_bits() const { return detail::multiply(dram_chips, dram_chip.bank.data_bits); }
    std::size_t word_count() const { return dram_chip.word_count(); }
    std::size_t capacity_bits() const { return detail::multiply(word_count(), word_bits()); }
    std::size_t capacity_bytes() const { return detail::bytes_for_bits(capacity_bits()); }
};

struct DIMMGeometry {
    std::size_t ranks = 4;
    RankGeometry rank{};

    void validate() const {
        rank.validate();
        if (ranks == 0) throw std::invalid_argument("DIMM requires at least one rank");
        (void)capacity_bits();
    }
    std::size_t word_bits() const { return rank.word_bits(); }
    std::size_t word_count() const { return detail::multiply(ranks, rank.word_count()); }
    std::size_t capacity_bits() const { return detail::multiply(word_count(), word_bits()); }
    std::size_t capacity_bytes() const { return detail::bytes_for_bits(capacity_bits()); }
};

struct DRAMChipAddress {
    std::size_t bank;
    std::size_t row;
    std::size_t column;
};

struct DIMMAddress {
    std::size_t rank;
    std::size_t bank;
    std::size_t row;
    std::size_t column;
};

} // namespace colab::dimm
