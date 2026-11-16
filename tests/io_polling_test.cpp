#include "io_test_support.hpp"
using namespace io_test;

int main() {
    // The CPU executes the driver. No host-side DATA reads or copies.
    for (const auto& cpu_config : cpu_configs()) {
        const auto image = examples::guest::hdd_read(Mode::Polling);
        SoC system(image.instructions, cpu_config); image.load_constants(system.memory()); seed_hdd(system);
        run_guest(system);
        assert(system.memory().read(0x800, 512) == pattern());
        assert(system.hdd().state() == devices::hdd::DeviceState::Completed);
        assert(system.hdd().dma().state() == c::DmaState::Idle);
        assert(system.cpu().interrupts().entries() == 0);
        assert(system.cpu().bus_waits() > 0);
    }
    SoC system({0x00000063}, config()); seed_hdd(system);
    write(system, SoC::HddBase + Block::Length, 512);
    write(system, SoC::HddBase + Block::Command, Block::Read);
    assert(read(system, SoC::HddBase + Block::Status) == Block::Busy);
    const auto early = transfer(system, SoC::HddBase + Block::Data, false);
    assert(early.status == interconnect::pcie::Status::BackendError);
    until(system, [&] { return system.hdd().state() == devices::hdd::DeviceState::DataReady; });
    for (unsigned i = 0; i < 50; ++i) system.tick();
    assert(system.memory().read(0x800, 512) == c::Bytes(512)); // PIO cannot autonomously write host RAM.
    assert(read(system, SoC::HddBase + Block::Data) == c::load32(pattern()));
    std::cout << "PASS polling: guest LW/SW, busy/ready handshake, no implicit DMA\n";
}
