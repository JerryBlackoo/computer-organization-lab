#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace colab::soc::core::mmu {

// Shared organization, not an ISA requirement. Defaults preserve 16-entry FA TLBs.
struct TlbConfig {
    std::size_t entries = 16;
    std::size_t ways = 16;
};

class TlbGeometry {
public:
    explicit TlbGeometry(TlbConfig config = {}) : config_(config) {
        if (!config.entries || !config.ways || config.ways > config.entries || config.entries % config.ways)
            throw std::invalid_argument("TLB requires entries = sets * ways with positive entries/ways");
        const auto count = sets();
        if ((count & (count - 1)) || count > (std::size_t{1} << 20))
            throw std::invalid_argument("TLB set count must be a power of two within the 20-bit VPN space");
    }
    const TlbConfig& config() const { return config_; }
    std::size_t capacity() const { return config_.entries; }
    std::size_t ways() const { return config_.ways; }
    std::size_t sets() const { return config_.entries / config_.ways; }
    // key is VPN for 4KiB pages, VPN[1] for Sv32 4MiB pages.
    std::size_t set_index(std::uint32_t key) const { return key & (sets() - 1); }
    std::size_t first(std::size_t set) const { return set * ways(); }
private:
    TlbConfig config_;
};

} // namespace colab::soc::core::mmu
