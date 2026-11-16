#pragma once

#include "../common/mmio_device.hpp"
#include "../common/irq_sink.hpp"
#include <deque>
#include <string>

namespace colab::devices {

// Teaching byte UART over 32-bit MMIO; not a 16550-compatible device.
class Uart final : public common::MmioDevice {
public:
    enum Register : std::uint32_t { Data = 0, Status = 4, Control = 8 };
    static constexpr std::uint32_t RxReady = 1, TxReady = 2, TxIdle = 4;
    static constexpr std::uint32_t RxIrq = 1, TxIrq = 2;
    static constexpr std::size_t FifoCapacity = 16, OutputCapacity = 4096;
    Uart(common::IrqSink& irq, unsigned source) : irq_(irq), source_(source) {}

    std::uint32_t mmio_read32(std::uint32_t offset) override {
        switch (offset) {
        case Data: {
            if (rx_.empty()) throw std::underflow_error("UART receive FIFO empty");
            const auto byte = rx_.front(); rx_.pop_front(); update_irq(); return byte;
        }
        case Status: return (rx_.empty() ? 0u : RxReady) | (tx_.size() < FifoCapacity ? TxReady : 0u)
                          | (tx_.empty() ? TxIdle : 0u);
        case Control: return control_;
        default: throw std::out_of_range("unknown UART register");
        }
    }
    void mmio_write32(std::uint32_t offset, std::uint32_t value) override {
        if (offset == Data) {
            if (tx_.size() == FifoCapacity) throw std::overflow_error("UART transmit FIFO full");
            tx_.push_back(static_cast<std::uint8_t>(value)); update_irq(); return;
        }
        if (offset == Control) {
            if (value & ~(RxIrq | TxIrq)) throw std::invalid_argument("unknown UART control bits");
            control_ = value; update_irq(); return;
        }
        throw std::out_of_range("read-only or unknown UART register");
    }
    void tick() override {
        if (tx_.empty() || output_.size() == OutputCapacity) return;
        if (++elapsed_ < 4) return; // Four enabled system cycles per transmitted byte.
        elapsed_ = 0; output_.push_back(static_cast<char>(tx_.front())); tx_.pop_front(); update_irq();
    }
    void inject(const std::string& bytes) {
        if (bytes.size() > FifoCapacity - rx_.size()) throw std::overflow_error("UART receive FIFO full");
        for (unsigned char byte : bytes) rx_.push_back(byte);
        update_irq();
    }
    std::string take_output() { std::string result; result.swap(output_); return result; }
    std::size_t output_size() const { return output_.size(); }
private:
    void update_irq() { irq_.set_irq(source_, ((control_ & RxIrq) && !rx_.empty()) ||
                                              ((control_ & TxIrq) && tx_.empty())); }
    common::IrqSink& irq_;
    unsigned source_;
    std::uint32_t control_ = 0, elapsed_ = 0;
    std::deque<std::uint8_t> rx_, tx_;
    std::string output_;
};
} // namespace colab::devices
