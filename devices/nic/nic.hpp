#pragma once

#include "../common/dma_bus.hpp"
#include "../common/irq_sink.hpp"

namespace colab::devices {
// Small loopback NIC for multi-master experiments, not a real Ethernet controller.
class NIC final : public common::MmioDevice {
public:
    enum Register : std::uint32_t { TxAddress = 0, RxAddress = 4, Length = 8, Command = 12, Status = 16, Irq = 20 };
    NIC(common::DmaBus& bus, common::Master master, common::IrqSink& irq, unsigned source)
        : dma_(bus, master, 32), irq_(irq), source_(source) {}
    std::uint32_t mmio_read32(std::uint32_t offset) override {
        switch (offset) {
        case TxAddress: return tx_;
        case RxAddress: return rx_;
        case Length: return length_;
        case Status: return status_;
        case Irq: return pending_;
        default: throw std::out_of_range("unknown NIC register");
        }
    }
    void mmio_write32(std::uint32_t offset, std::uint32_t value) override {
        if (offset == Irq) { if (value & 1) { pending_ = false; irq_.set_irq(source_, false); } return; }
        if (status_ == 1) throw std::logic_error("NIC busy");
        switch (offset) {
        case TxAddress: tx_ = value; break;
        case RxAddress: rx_ = value; break;
        case Length: length_ = value; break;
        case Command:
            if (value != 1 || !length_ || length_ > 4096) throw std::invalid_argument("invalid NIC loopback command");
            pending_ = false; irq_.set_irq(source_, false); status_ = 1; receiving_ = false;
            dma_.start({tx_, length_, common::DmaDirection::MemoryToDevice}); break;
        default: throw std::out_of_range("read-only/unknown NIC register");
        }
    }
    void tick() override {
        dma_.tick();
        if (status_ != 1) return;
        if (dma_.failed()) { finish(4); return; }
        if (!dma_.completed()) return;
        if (receiving_) finish(2);
        else {
            receiving_ = true;
            dma_.start({rx_, length_, common::DmaDirection::DeviceToMemory}, dma_.data());
        }
    }
private:
    void finish(std::uint32_t status) { status_ = status; pending_ = true; irq_.set_irq(source_, true); }
    common::DmaEngine dma_;
    common::IrqSink& irq_;
    unsigned source_;
    std::uint32_t tx_ = 0, rx_ = 0, length_ = 0, status_ = 0;
    bool receiving_ = false, pending_ = false;
};
}
