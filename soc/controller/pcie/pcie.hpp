#pragma once

#include "../../../interconnect/interconnect.hpp"
#include "../../../interconnect/pcie/endpoint.hpp"

namespace colab::soc::controller {
// Host bridge for the board's statically configured PCIe memory windows.
class PcieRootComplex {
public:
    explicit PcieRootComplex(colab::Interconnect& bus) : bus_(bus) {}

    static pcie::MemoryRead memory_read(std::uint32_t address, std::size_t count) {
        check_word_access(address, count);
        return {address};
    }

    static pcie::MemoryWrite memory_write(std::uint32_t address, const devices::common::Bytes& bytes) {
        check_word_access(address, bytes.size());
        return {address, devices::common::load32(bytes)};
    }

    void attach(std::uint32_t bar, std::uint64_t size, devices::common::MmioDevice& registers) {
        const pcie::BarEndpoint endpoint(bar, size, registers);
        bus_.map_mmio_endpoint({bar, size, 1,
            [endpoint](auto address, auto count) {
                return devices::common::word_bytes(endpoint.receive(memory_read(address, count)));
            },
            [endpoint](auto address, const auto& bytes) {
                endpoint.receive(memory_write(address, bytes));
            }, "PCIe memory BAR"});
    }

private:
    static void check_word_access(std::uint32_t address, std::size_t count) {
        if (count != 4 || address % 4)
            throw std::invalid_argument("PCIe MMIO requires one aligned 32-bit word");
    }

    colab::Interconnect& bus_;
};

using PCIe = PcieRootComplex;
}
