#pragma once

#include "../../../interconnect/interconnect.hpp"

namespace colab::soc::controller {
// Statically configured BAR windows; transport timing/arbitration live in the shared fabric.
// Enumeration/configuration TLPs, MSI-X tables and the electrical link are not modeled.
class PCIe {
public:
    explicit PCIe(colab::Interconnect& bus) : bus_(bus) {}
    void attach(std::uint32_t bar, std::uint64_t size, devices::common::MmioDevice& endpoint) {
        bus_.map_mmio(bar, size, endpoint);
    }
private:
    colab::Interconnect& bus_;
};
}
