#include "../devices/uart/uart.hpp"
#include <iostream>

struct Irq final : colab::devices::common::IrqSink {
    bool asserted = false;
    void set_irq(unsigned source, bool value) override {
        if (source != 5) throw std::runtime_error("unexpected IRQ source");
        asserted = value;
    }
};
static void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

int main() {
    using U = colab::devices::Uart;
    Irq irq;
    U uart(irq, 5);
    uart.mmio_write32(U::Control, U::TxIrq | U::RxIrq);
    require(irq.asserted, "empty transmitter did not raise level IRQ");
    uart.mmio_write32(U::Data, 0x141);
    require(!irq.asserted, "nonempty transmitter did not deassert IRQ");
    for (int i = 0; i < 3; ++i) uart.tick();
    require(uart.output_size() == 0, "byte completed before four cycles");
    uart.tick();
    require(uart.take_output() == "A" && irq.asserted, "TX byte/timing/IRQ wrong");
    uart.mmio_write32(U::Control, U::RxIrq);
    uart.inject("xy");
    require(irq.asserted && uart.mmio_read32(U::Data) == 'x' && irq.asserted, "RX level IRQ cleared too early");
    require(uart.mmio_read32(U::Data) == 'y' && !irq.asserted, "RX IRQ did not clear after drain");
    bool empty_error = false;
    try { uart.mmio_read32(U::Data); } catch (const std::underflow_error&) { empty_error = true; }
    require(empty_error, "empty FIFO read succeeded");
    for (std::size_t i = 0; i < U::OutputCapacity; ++i) {
        uart.mmio_write32(U::Data, 'a');
        for (int j = 0; j < 4; ++j) uart.tick();
    }
    uart.mmio_write32(U::Data, 'z');
    for (int j = 0; j < 8; ++j) uart.tick();
    require(uart.output_size() == U::OutputCapacity && !(uart.mmio_read32(U::Status) & U::TxIdle),
            "full host output did not apply backpressure");
    require(uart.take_output() == std::string(U::OutputCapacity, 'a'), "transmitted bytes lost or reordered");
    for (int j = 0; j < 4; ++j) uart.tick();
    require(uart.take_output() == "z", "transmitter did not resume after host drain");
    for (std::size_t i = 0; i < U::FifoCapacity; ++i) uart.mmio_write32(U::Data, 'b');
    bool full_error = false;
    try { uart.mmio_write32(U::Data, 'c'); } catch (const std::overflow_error&) { full_error = true; }
    require(full_error && !(uart.mmio_read32(U::Status) & U::TxReady), "TX FIFO overflow was not reported");
    std::cout << "PASS UART: IRQ levels, FIFO bounds, timed transmission and output backpressure\n";
}
