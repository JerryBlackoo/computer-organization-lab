#pragma once

#include "controller/controller.hpp"

namespace colab::devices {
class NVMe final : public common::MmioDevice {
public:
    NVMe(common::DmaBus& bus, common::Master master, common::IrqSink& irq, unsigned source)
        : controller_(nand_, bus, master, irq, source) {}
    nvme::NAND& nand() { return nand_; } // host image/fixture initialization only
    const nvme::Controller& controller() const { return controller_; }
    std::uint32_t mmio_read32(std::uint32_t offset) override { return controller_.mmio_read32(offset); }
    void mmio_write32(std::uint32_t offset, std::uint32_t value) override { controller_.mmio_write32(offset, value); }
    void tick() override { controller_.tick(); }
private:
    nvme::NAND nand_;
    nvme::Controller controller_;
};
}
