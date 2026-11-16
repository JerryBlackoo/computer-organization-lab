#pragma once

#include "../common/mmio_device.hpp"
#include <limits>

namespace colab::devices {
// Single-hart CLINT-compatible register layout. No PLIC claim/complete path:
// these level signals go directly to the CPU's MTIP/MSIP inputs.
class Clint final : public common::MmioDevice {
public:
    enum Register : std::uint32_t { Msip = 0, MtimecmpLow = 0x4000, MtimecmpHigh = 0x4004,
                                   MtimeLow = 0xbff8, MtimeHigh = 0xbffc };
    std::uint32_t mmio_read32(std::uint32_t offset) override {
        switch (offset) {
        case Msip: return software_ ? 1 : 0;
        case MtimecmpLow: return static_cast<std::uint32_t>(compare_);
        case MtimecmpHigh: return static_cast<std::uint32_t>(compare_ >> 32);
        case MtimeLow: return static_cast<std::uint32_t>(time_);
        case MtimeHigh: return static_cast<std::uint32_t>(time_ >> 32);
        default: throw std::out_of_range("unknown single-hart CLINT register");
        }
    }
    void mmio_write32(std::uint32_t offset, std::uint32_t value) override {
        switch (offset) {
        case Msip: software_ = (value & 1) != 0; break;
        case MtimecmpLow: compare_ = (compare_ & 0xffffffff00000000ull) | value; break;
        case MtimecmpHigh: compare_ = (std::uint64_t(value) << 32) | std::uint32_t(compare_); break;
        case MtimeLow: time_ = (time_ & 0xffffffff00000000ull) | value; break;
        case MtimeHigh: time_ = (std::uint64_t(value) << 32) | std::uint32_t(time_); break;
        default: throw std::out_of_range("unknown single-hart CLINT register");
        }
    }
    void tick() override { ++time_; }
    std::uint64_t time() const { return time_; }
    std::uint64_t compare() const { return compare_; }
    bool timer_pending() const { return time_ >= compare_; }
    bool software_pending() const { return software_; }
private:
    std::uint64_t time_ = 0, compare_ = std::numeric_limits<std::uint64_t>::max();
    bool software_ = false;
};
} // namespace colab::devices
