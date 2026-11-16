#include "io_test_support.hpp"
using namespace io_test;

static void ack(SoC& system) { write(system, SoC::HddBase + Block::IrqStatus, 1); }
static void wait_ready(SoC& system) {
    until(system, [&] { return system.hdd().state() == devices::hdd::DeviceState::DataReady; });
    if (!read(system, SoC::HddBase + Block::IrqStatus)) throw std::runtime_error("missing ready IRQ");
    ack(system);
}
static void test_read_submission_loss() {
    SoC system({0x00000063}, config()); seed_hdd(system);
    auto& hdd = system.hdd();
    hdd.mmio_write32(Block::Length, 512);
    hdd.mmio_write32(Block::Control, Block::EnableIrq);
    hdd.mmio_write32(Block::Command, Block::Read);
    system.disk().power_off();
    hdd.tick();
    if (hdd.mmio_read32(Block::Status) != Block::Failed || hdd.mmio_read32(Block::Progress) != 0 ||
        hdd.mmio_read32(Block::IrqStatus) != 1 || hdd.mmio_read32(Block::ErrorCode) != 1 ||
        !system.interrupts().requested()) throw std::runtime_error("read submit loss was not reported");
    hdd.mmio_write32(Block::IrqStatus, 1);
    system.disk().power_on(); system.disk().advance(system.disk().config().spin_up_ms);
    hdd.mmio_write32(Block::Command, Block::Read);
    until(system, [&] { return hdd.state() == devices::hdd::DeviceState::DataReady; });
    if (hdd.mmio_read32(Block::Data) != c::load32(pattern(), 0)) throw std::runtime_error("read recovery failed");
}
static void test_dma_write_submission_loss() {
    SoC system({0x00000063}, config()); seed_hdd(system);
    system.memory().write(0x800, pattern());
    auto& hdd = system.hdd();
    hdd.mmio_write32(Block::Length, 512);
    hdd.mmio_write32(Block::DmaAddress, 0x800);
    hdd.mmio_write32(Block::Control, Block::EnableIrq | Block::EnableDma);
    hdd.mmio_write32(Block::Command, Block::Write);
    system.disk().power_off();
    until(system, [&] { return hdd.state() == devices::hdd::DeviceState::Error; });
    if (hdd.mmio_read32(Block::Progress) != 512 || hdd.mmio_read32(Block::IrqStatus) != 1 ||
        hdd.mmio_read32(Block::ErrorCode) != 1 || !system.interrupts().requested())
        throw std::runtime_error("DMA write submit loss was not reported");
    hdd.mmio_write32(Block::IrqStatus, 1);
    system.disk().power_on(); system.disk().advance(system.disk().config().spin_up_ms);
    hdd.mmio_write32(Block::Command, Block::Write);
    until(system, [&] { return hdd.state() == devices::hdd::DeviceState::Completed; });
    if (hdd.mmio_read32(Block::Progress) != 512) throw std::runtime_error("DMA recovery failed");
}
static void test_pio_write_loss() {
    SoC system({0x00000063}, config()); seed_hdd(system);
    write(system, SoC::HddBase + Block::Length, 512);
    write(system, SoC::HddBase + Block::Control, Block::EnableIrq);
    write(system, SoC::HddBase + Block::Command, Block::Write);
    const auto data = pattern();
    for (unsigned offset = 0; offset < 508; offset += 4) {
        wait_ready(system); write(system, SoC::HddBase + Block::Data, c::load32(data, offset));
    }
    wait_ready(system);
    if (read(system, SoC::HddBase + Block::Progress) != 508) throw std::runtime_error("wrong pre-submit progress");
    system.disk().power_off();
    const auto last = transfer(system, SoC::HddBase + Block::Data, true, c::load32(data, 508));
    for (unsigned i = 0; i < 10; ++i) system.tick();
    const auto state = read(system, SoC::HddBase + Block::Status);
    const auto progress = read(system, SoC::HddBase + Block::Progress);
    const auto irq = read(system, SoC::HddBase + Block::IrqStatus);
    const auto error = read(system, SoC::HddBase + Block::ErrorCode);
    if (last.status != interconnect::pcie::Status::Success || state != Block::Failed || progress != 512 ||
        irq != 1 || error != 1 || !system.interrupts().requested()) {
        std::cerr << "FAIL PIO lost power: bus=" << int(last.status) << " status=" << state
                  << " progress=" << progress << " irq=" << irq << " error=" << error << '\n';
        throw std::runtime_error("PIO write submit loss was not reported");
    }
    ack(system);
    if (system.interrupts().requested()) throw std::runtime_error("IRQ acknowledgment failed");
    system.disk().power_on(); system.disk().advance(system.disk().config().spin_up_ms);
    write(system, SoC::HddBase + Block::Command, Block::Write);
    for (unsigned offset = 0; offset < 512; offset += 4) {
        wait_ready(system); write(system, SoC::HddBase + Block::Data, c::load32(data, offset));
    }
    until(system, [&] { return system.hdd().state() == devices::hdd::DeviceState::Completed; });
    if (read(system, SoC::HddBase + Block::Progress) != 512 || !read(system, SoC::HddBase + Block::IrqStatus))
        throw std::runtime_error("recovery write incomplete");
    ack(system);
    write(system, SoC::HddBase + Block::Command, Block::Read);
    for (unsigned offset = 0; offset < 512; offset += 4) {
        wait_ready(system);
        if (read(system, SoC::HddBase + Block::Data) != c::load32(data, offset)) throw std::runtime_error("recovery read differs");
    }
    if (read(system, SoC::HddBase + Block::Status) != Block::Done || !read(system, SoC::HddBase + Block::IrqStatus))
        throw std::runtime_error("recovery read incomplete");
}
int main() {
    test_pio_write_loss(); test_read_submission_loss(); test_dma_write_submission_loss();
    std::cout << "PASS HDD PIO, read and DMA submission power loss, IRQ and recovery\n";
}
