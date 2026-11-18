#pragma once

#include "../../devices/common/mmio_device.hpp"

namespace colab::pcie {

// Transaction-level subset: one aligned DWORD, with all four byte lanes enabled.
struct MemoryRead {
    std::uint32_t address;
};

struct MemoryWrite {
    std::uint32_t address;
    std::uint32_t data;
};

class BarEndpoint {
public:
    BarEndpoint(std::uint32_t base, std::uint64_t size, devices::common::MmioDevice& registers)
        : bar_base_(base), bar_size_(size), registers_(registers) {
        if (base % 4 || size < 4 || size > (std::uint64_t{1} << 32) - base)
            throw std::invalid_argument("invalid PCIe memory BAR window");
    }

    std::uint32_t receive(const MemoryRead& request) const {
        return registers_.mmio_read32(decode_bar(request.address));
    }

    void receive(const MemoryWrite& request) const {
        const auto offset = decode_bar(request.address);
        registers_.mmio_write32(offset, request.data);
    }

private:
    std::uint32_t decode_bar(std::uint32_t address) const {
        if (address % 4) throw std::invalid_argument("unaligned PCIe memory request");
        if (address < bar_base_ || std::uint64_t(address) - bar_base_ > bar_size_ - 4)
            throw std::out_of_range("PCIe memory request did not match the device BAR");
        return address - bar_base_;
    }

    std::uint32_t bar_base_;
    std::uint64_t bar_size_;
    devices::common::MmioDevice& registers_;
};

} // namespace colab::pcie
