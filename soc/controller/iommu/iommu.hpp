#pragma once

#include "../../../devices/common/dma_bus.hpp"

namespace colab::soc::controller {
// Teaching aperture IOMMU, distinct from the CPU MMU. Disabled = physical addresses.
class IOMMU {
public:
    using Master = devices::common::Master;
    struct Mapping {
        Master master;
        std::uint64_t iova;
        std::uint32_t physical;
        std::uint64_t size;
        bool readable, writable;
    };
    void enable(bool value = true) { enabled_ = value; }
    bool enabled() const { return enabled_; }
    void map(Mapping mapping) {
        constexpr auto space = std::uint64_t{1} << 32;
        if (!mapping.size || mapping.size > space - mapping.physical ||
            mapping.iova > UINT64_MAX - (mapping.size - 1)) throw std::invalid_argument("invalid IOMMU aperture");
        for (const auto& old : mappings_) {
            if (old.master == mapping.master &&
                (mapping.iova >= old.iova ? mapping.iova - old.iova < old.size : old.iova - mapping.iova < mapping.size))
                throw std::invalid_argument("overlapping IOMMU apertures");
        }
        mappings_.push_back(mapping);
    }
    std::uint32_t translate(Master master, std::uint64_t address, std::size_t size, bool write) const {
        if (!size || address > UINT64_MAX - (size - 1)) throw std::out_of_range("DMA address overflow");
        if (!enabled_) {
            if (address >= (std::uint64_t{1} << 32) || size > (std::uint64_t{1} << 32) - address)
                throw std::out_of_range("DMA physical address exceeds RV32");
            return static_cast<std::uint32_t>(address);
        }
        for (const auto& item : mappings_) {
            if (item.master != master || address < item.iova || address - item.iova >= item.size ||
                size > item.size - (address - item.iova)) continue;
            if (write ? !item.writable : !item.readable) throw std::runtime_error("IOMMU permission denied");
            return static_cast<std::uint32_t>(item.physical + (address - item.iova));
        }
        throw std::out_of_range("IOMMU unmapped or crossing aperture");
    }
private:
    bool enabled_ = false;
    std::vector<Mapping> mappings_;
};
}
