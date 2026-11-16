#pragma once

#include "pcie/pcie.hpp"
#include "../devices/common/mmio_device.hpp"
#include "../soc/controller/iommu/iommu.hpp"
#include "../soc/controller/memory/memory.hpp"

namespace colab {
// One shared, delayed, round-robin transaction fabric for CPU and bus masters.
// CPU master 0 may access MMIO; DMA masters may access mapped DIMMs only.
class Interconnect final : public devices::common::DmaBus {
public:
    using Bytes = devices::common::Bytes;
    explicit Interconnect(soc::controller::IOMMU& iommu, interconnect::pcie::Config config = {})
        : iommu_(iommu), fabric_(config) {}
    // Adapter endpoints preserve the existing fabric/IOMMU while using TLM ports.
    void map_memory_endpoint(interconnect::pcie::Endpoint endpoint) {
        const Range range{endpoint.base, endpoint.size};
        fabric_.map(std::move(endpoint));
        ram_.push_back(range);
    }
    void map_mmio_endpoint(interconnect::pcie::Endpoint endpoint) {
        const Range range{endpoint.base, endpoint.size};
        fabric_.map(std::move(endpoint));
        mmio_.push_back(range);
    }
    void map_memory(std::uint32_t base, std::uint64_t size, soc::controller::Memory& memory,
                    unsigned latency = 2) {
        fabric_.map({base, size, latency,
            [&memory](auto address, auto count) { return memory.read(address, count); },
            [&memory](auto address, const auto& data) { memory.write(address, data); }, "DIMM"});
        ram_.push_back({base, size});
    }
    void map_mmio(std::uint32_t base, std::uint64_t size, devices::common::MmioDevice& device) {
        fabric_.map({base, size, 1,
            [&device, base](auto address, auto count) {
                if (count != 4 || address % 4) throw std::invalid_argument("MMIO requires aligned 32-bit access");
                return devices::common::word_bytes(device.mmio_read32(address - base));
            },
            [&device, base](auto address, const auto& data) {
                if (data.size() != 4 || address % 4) throw std::invalid_argument("MMIO requires aligned 32-bit access");
                device.mmio_write32(address - base, devices::common::load32(data));
            }, "MMIO"});
        mmio_.push_back({base, size});
    }
    devices::common::Tag submit(devices::common::Master master, devices::common::BusOperation operation,
                                std::uint64_t address, std::size_t count, Bytes data = {}) override {
        if (!count || count > 1024 || (operation == devices::common::BusOperation::Write ? data.size() != count : !data.empty()))
            throw std::invalid_argument("invalid system transaction");
        std::uint32_t physical;
        if (master == 0) {
            if (address >= (std::uint64_t{1} << 32) || count > (std::uint64_t{1} << 32) - address)
                throw std::out_of_range("CPU physical address overflow");
            physical = static_cast<std::uint32_t>(address);
            for (const auto& range : mmio_) {
                if (std::uint64_t(physical) < std::uint64_t(range.base) + range.size &&
                    std::uint64_t(range.base) < std::uint64_t(physical) + count &&
                    (count != 4 || physical % 4))
                    throw std::invalid_argument("MMIO transaction must be one aligned 32-bit register");
            }
        } else {
            physical = iommu_.translate(master, address, count, operation == devices::common::BusOperation::Write);
            bool mapped = false;
            for (const auto& range : ram_) {
                if (physical >= range.base && physical - range.base < range.size &&
                    count <= range.size - (physical - range.base)) mapped = true;
            }
            if (!mapped) throw std::out_of_range("DMA target is not mapped RAM");
        }
        // Naturally aligned CPU halfwords are one data beat, preserving atomic
        // visibility to DMA observers. MMIO was already restricted to full words.
        const auto width = physical % 4 == 0 && count % 4 == 0 ? 4u :
            master == 0 && count == 2 && physical % 2 == 0 ? 2u : 1u;
        return fabric_.submit({master, operation, physical, width, count / width, true, std::move(data)});
    }
    bool ready(devices::common::Tag tag) const override { return fabric_.ready(tag); }
    devices::common::BusResult take(devices::common::Tag tag) override { return fabric_.take(tag); }
    void tick() { fabric_.tick(); }
    bool idle() const { return fabric_.idle(); }
    const interconnect::pcie::Fabric& fabric() const { return fabric_; }
    bool is_memory(std::uint32_t address, std::size_t size) const override {
        if (!size) return false;
        for (const auto& range : ram_) {
            if (address >= range.base && std::uint64_t{address} - range.base < range.size &&
                size <= range.size - (std::uint64_t{address} - range.base)) return true;
        }
        return false;
    }
private:
    struct Range { std::uint32_t base; std::uint64_t size; };
    soc::controller::IOMMU& iommu_;
    interconnect::pcie::Fabric fabric_;
    std::vector<Range> ram_, mmio_;
};
}
