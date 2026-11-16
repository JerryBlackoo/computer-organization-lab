#pragma once

#include "../common/mmio_device.hpp"
#include "../common/irq_sink.hpp"

namespace colab::devices {

// Teaching timer: interrupt-controller source, not the RISC-V CLINT/mtime interface.
class Timer final : public common::MmioDevice {
public:
    enum Register : std::uint32_t { Count = 0, Compare = 4, Control = 8, Pending = 12 };
    static constexpr std::uint32_t Enable = 1, Periodic = 2, IrqEnable = 4;
    Timer(common::IrqSink& irq, unsigned source) : irq_(irq), source_(source) {}

    std::uint32_t mmio_read32(std::uint32_t offset) override {
        switch (offset) {
        case Count: return count_;
        case Compare: return compare_;
        case Control: return control_;
        case Pending: return pending_ ? 1 : 0;
        default: throw std::out_of_range("unknown timer register");
        }
    }
    void mmio_write32(std::uint32_t offset, std::uint32_t value) override {
        switch (offset) {
        case Count:
            if (control_ & Enable) throw std::logic_error("disable timer before setting count");
            count_ = value; break;
        case Compare:
            if (control_ & Enable) throw std::logic_error("disable timer before setting compare");
            compare_ = value; break;
        case Control:
            if (value & ~(Enable | Periodic | IrqEnable)) throw std::invalid_argument("unknown timer control bits");
            if ((value & Enable) && !compare_) throw std::invalid_argument("timer compare must be positive");
            control_ = value; update_irq(); break;
        case Pending:
            if (value & ~1u) throw std::invalid_argument("unknown timer acknowledgement bits");
            if (value & 1) pending_ = false;
            update_irq(); break;
        default: throw std::out_of_range("unknown timer register");
        }
    }
    void tick() override {
        if (!(control_ & Enable)) return;
        // Compare before increment so even a near-UINT32_MAX starting value cannot wrap silently.
        if (count_ >= compare_ - 1) {
            pending_ = true;
            if (control_ & Periodic) count_ = 0;
            else { count_ = compare_; control_ &= ~Enable; }
            update_irq();
        } else ++count_;
    }
private:
    void update_irq() { irq_.set_irq(source_, pending_ && (control_ & IrqEnable)); }
    common::IrqSink& irq_;
    unsigned source_;
    std::uint32_t count_ = 0, compare_ = 0, control_ = 0;
    bool pending_ = false;
};
} // namespace colab::devices
