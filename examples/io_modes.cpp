#include "guest/io_driver.hpp"
#include "../simulation/simulator.hpp"
#include <iostream>

int main() {
    using namespace colab;
    using namespace examples::guest;
    for (const auto mode : {IoMode::Polling, IoMode::Interrupt, IoMode::DmaPolling, IoMode::DmaInterrupt}) {
        const auto image = hdd_read(mode);
        SoCConfig config; config.disk.spin_up_ms = 1; config.disk.tick_ms = 0.05L;
        SoC system(image.instructions, config); image.load_constants(system.memory());
        devices::common::Bytes expected(512);
        for (std::size_t i = 0; i < expected.size(); ++i) expected[i] = static_cast<std::uint8_t>(i * 73 + 5);
        // Initialize the disk image before starting the guest; not part of its I/O path.
        system.disk().power_on(); system.disk().advance(config.disk.spin_up_ms);
        system.disk().submit_write_lba(0, expected);
        if (!system.disk().run_until_idle(100) || !system.disk().take_completion()) return 1;
        simulation::Simulator<SoC> simulator(system);
        while (devices::common::load32(system.memory().read(0x10, 4)) != 1 && simulator.cycles() < 200000)
            simulator.step();
        if (devices::common::load32(system.memory().read(0x10, 4)) != 1 || system.memory().read(0x800, 512) != expected)
            throw std::runtime_error("guest I/O did not complete correctly");
        const char* names[]{"Polling/PIO", "Interrupt/PIO", "DMA/polling", "DMA/interrupt"};
        std::cout << names[static_cast<unsigned>(mode)] << ": bytes=512 equal=yes cycles=" << simulator.cycles()
                  << " retired=" << system.cpu().completed_instructions()
                  << " IRQs=" << system.cpu().interrupts().entries()
                  << " DMA-bytes=" << system.hdd().dma().transferred() << '\n';
    }
}
