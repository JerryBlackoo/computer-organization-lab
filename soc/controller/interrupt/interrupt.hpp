#pragma once

#include "../../../devices/common/mmio_device.hpp"
#include "../../../devices/common/irq_sink.hpp"

namespace colab::soc::controller {
// Level-triggered sources 1..31, fixed response priority (smaller ID first).
// MASK bit 1 = masked, as in the textbook. This is not a full PLIC.
class Interrupt final : public devices::common::MmioDevice, public devices::common::IrqSink {
public:
    static constexpr std::uint32_t Pending = 0, Mask = 4, Claim = 8, Complete = 12;
    void set_irq(unsigned source, bool asserted) override {
        if (!source || source >= 32) throw std::out_of_range("invalid IRQ source");
        const auto bit = std::uint32_t{1} << source;
        if (asserted) { levels_ |= bit; if (!(in_service_ & bit)) pending_ |= bit; }
        else { levels_ &= ~bit; pending_ &= ~bit; }
    }
    bool requested() const { return (pending_ & ~mask_) != 0; }
    std::uint32_t mmio_read32(std::uint32_t offset) override {
        if (offset == Pending) return pending_;
        if (offset == Mask) return mask_;
        if (offset == Claim) {
            for (unsigned id = 1; id < 32; ++id) {
                const auto bit = std::uint32_t{1} << id;
                if (pending_ & ~mask_ & bit) { pending_ &= ~bit; in_service_ |= bit; return id; }
            }
            return 0;
        }
        throw std::out_of_range("invalid interrupt register read");
    }
    void mmio_write32(std::uint32_t offset, std::uint32_t value) override {
        if (offset == Mask) { mask_ = value & ~1u; return; }
        if (offset != Complete || !value || value >= 32) throw std::invalid_argument("invalid IRQ completion");
        const auto bit = std::uint32_t{1} << value;
        if (!(in_service_ & bit)) throw std::logic_error("IRQ was not claimed");
        in_service_ &= ~bit;
        if (levels_ & bit) pending_ |= bit;
    }
    void tick() override {}
private:
    std::uint32_t pending_ = 0, levels_ = 0, in_service_ = 0, mask_ = 0;
};
}
