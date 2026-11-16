#pragma once

#include "../types.hpp"

#include <limits>
#include <stdexcept>

namespace colab::soc::cache {

class AddressDecoder {
public:
    explicit AddressDecoder(Config config) : config_(config) {
        if (!power_of_two(config.sets) || !power_of_two(config.line_bytes) || config.ways == 0) {
            throw std::invalid_argument("cache requires power-of-two sets/line bytes and ways > 0");
        }
        if (offset_bits() > 32 || index_bits() > 32 || offset_bits() + index_bits() > 32) {
            throw std::invalid_argument("cache index and offset exceed 32-bit physical address");
        }
        if (config.ways > std::numeric_limits<std::size_t>::max() / config.sets
            || config.ways * config.sets > std::numeric_limits<std::size_t>::max() / config.line_bytes) {
            throw std::invalid_argument("cache capacity overflows size_t");
        }
        // 后续容量报告按 bit 计数；提前拒绝无法表示的几何配置。
        if (capacity_bytes() > std::numeric_limits<std::uint64_t>::max() / 256) {
            throw std::invalid_argument("cache bit accounting overflows uint64_t");
        }
    }

    // PIPT：物理地址分成 Tag / 组号 / 块内偏移。
    AddressParts decode(Address address) const {
        const auto block = std::uint64_t{address} / config_.line_bytes;
        return {static_cast<Address>(block / config_.sets),
                static_cast<std::size_t>(block % config_.sets),
                static_cast<std::size_t>(address % config_.line_bytes),
                static_cast<Address>(std::uint64_t{address} - address % config_.line_bytes),
                static_cast<Address>(block)};
    }

    Address block_address(Address tag, std::size_t set_index) const {
        if (set_index >= config_.sets) throw std::out_of_range("cache set index out of range");
        // 先检查 Tag 的范围，避免在地址重建时乘法溢出。
        const auto max_block = std::uint64_t{std::numeric_limits<Address>::max()} / config_.line_bytes;
        if (tag > max_block / config_.sets) throw std::out_of_range("cache tag out of range");
        const auto block = std::uint64_t{tag} * config_.sets + set_index;
        if (block > max_block) throw std::out_of_range("cache block out of range");
        return static_cast<Address>(block * config_.line_bytes);
    }

    const Config& config() const { return config_; }
    unsigned offset_bits() const { return log2(config_.line_bytes); }
    unsigned index_bits() const { return log2(config_.sets); }
    unsigned tag_bits() const { return 32 - offset_bits() - index_bits(); }
    std::size_t capacity_bytes() const { return config_.sets * config_.ways * config_.line_bytes; }

    Capacity capacity() const {
        const auto lines = std::uint64_t{config_.sets} * config_.ways;
        const auto rank_bits = ceil_log2(config_.ways);
        std::uint64_t replacement = 0;
        if (config_.ways > 1) {
            switch (config_.replacement_policy) {
            case ReplacementPolicy::LRU: replacement = lines * rank_bits; break;
            case ReplacementPolicy::FIFO: replacement = lines * rank_bits; break;
            case ReplacementPolicy::Random: replacement = std::uint64_t{config_.sets} * 32; break;
            case ReplacementPolicy::LFU: replacement = lines * (64 + rank_bits); break;
            }
        }
        return {std::uint64_t{capacity_bytes()} * 8, lines * tag_bits(), lines,
                config_.write_policy == WritePolicy::WriteBack ? lines : 0, replacement};
    }

private:
    static bool power_of_two(std::size_t value) { return value != 0 && (value & (value - 1)) == 0; }
    static unsigned log2(std::size_t value) {
        unsigned bits = 0;
        while (value > 1) { ++bits; value >>= 1; }
        return bits;
    }
    static unsigned ceil_log2(std::size_t value) {
        unsigned bits = 0;
        for (auto remaining = value - 1; remaining != 0; remaining >>= 1) ++bits;
        return bits;
    }

    Config config_;
};

} // namespace colab::soc::cache
