#include "../../simulation/systemc/adapters/clocked_soc.hpp"
#include "../guest/io_driver.hpp"
#include <iostream>
#include <memory>
#include <string>

using colab::simulation::systemc::ClockedSoC;
namespace common = colab::devices::common;

class CompletionMonitor : public sc_core::sc_module {
public:
    bool completed = false;
#if SC_VERSION_MAJOR < 3
    SC_HAS_PROCESS(CompletionMonitor);
#endif
    CompletionMonitor(sc_core::sc_module_name name, ClockedSoC& computer)
        : sc_core::sc_module(name), computer_(computer) {
        SC_METHOD(observe);
        // Wake after the SoC's snapshot signals update, not on the same edge.
        sensitive << computer.cycles;
        dont_initialize();
    }
private:
    void observe() {
        completed = common::load32(computer_.hardware().memory().read(0x10, 4)) == 1;
        if (completed) sc_core::sc_stop();
    }
    ClockedSoC& computer_;
};

int sc_main(int argc, char** argv) {
    const std::string device = argc > 1 ? argv[1] : "nvme";
    if (argc > 3 || (device != "nvme" && device != "hdd")) {
        std::cerr << "Usage: " << argv[0] << " [nvme|hdd] [VCD-path-without-extension]\n";
        return 2;
    }
    try {
        using namespace colab;
        const auto image = device == "nvme" ? examples::guest::nvme_read()
            : examples::guest::hdd_read(examples::guest::IoMode::DmaInterrupt);
        // Scaled teaching clock, not a claim about real CPU/PCIe frequency.
        const sc_core::sc_time period(50, sc_core::SC_US);
        SoCConfig config;
        config.disk.spin_up_ms = 1;
        config.disk.tick_ms = period.to_seconds() * 1000.0L;

        // First edge at one period: after N edges the kernel time is N*period.
        sc_core::sc_clock clock("clock", period, 0.5, period, true);
        ClockedSoC computer("computer", image.instructions, config);
        computer.clk(clock);
        auto& hardware = computer.hardware();
        image.load_constants(hardware.memory());
        common::Bytes expected(512);
        for (std::size_t i = 0; i < expected.size(); ++i)
            expected[i] = static_cast<std::uint8_t>(i * 73 + 5);

        // Fixture initialization before sc_start; guest I/O never uses this path.
        if (device == "nvme") hardware.nvme().nand().write(0, expected);
        else {
            hardware.disk().power_on();
            hardware.disk().advance(config.disk.spin_up_ms);
            hardware.disk().submit_write_lba(0, expected);
            if (!hardware.disk().run_until_idle(100)) throw std::runtime_error("disk fixture timed out");
            const auto completion = hardware.disk().take_completion();
            if (!completion || completion->status != devices::hdd::Status::Success)
                throw std::runtime_error("disk fixture write failed");
        }

        CompletionMonitor monitor("monitor", computer);
        std::unique_ptr<sc_core::sc_trace_file, decltype(&sc_core::sc_close_vcd_trace_file)>
            trace(nullptr, sc_core::sc_close_vcd_trace_file);
        if (argc == 3) {
            trace.reset(sc_core::sc_create_vcd_trace_file(argv[2]));
            if (!trace) throw std::runtime_error("cannot create VCD trace");
            trace->set_time_unit(1, sc_core::SC_NS);
            computer.trace(trace.get());
        }
        constexpr std::uint64_t cycle_limit = 200000;
        sc_core::sc_start(period * static_cast<double>(cycle_limit + 1));
        if (!monitor.completed) throw std::runtime_error("guest I/O exceeded simulation time limit");
        const std::uint32_t destination = device == "nvme" ? 0x5000 : 0x800;
        if (hardware.memory().read(destination, expected.size()) != expected ||
            hardware.cpu().registers().read(28) != common::load32(expected))
            throw std::runtime_error("guest data verification failed");
        if (device == "nvme" && hardware.nvme().controller().completed_commands() != 3)
            throw std::runtime_error("guest NVMe queue commands did not complete");
        if (device == "hdd" && (hardware.hdd().dma().transferred() != expected.size() ||
            !hardware.cpu().interrupts().entries() || !hardware.cpu().registers().read(9)))
            throw std::runtime_error("DMA/CPU concurrent work/interrupt verification failed");
        if (computer.cycles.read() != hardware.cycles() ||
            sc_core::sc_time_stamp() != period * static_cast<double>(hardware.cycles()))
            throw std::runtime_error("SystemC time and model cycles disagree");
        std::cout << "PASS SystemC " << device << ": bytes=512 equal=yes time=" << sc_core::sc_time_stamp()
                  << " cycles=" << hardware.cycles() << " retired=" << hardware.cpu().completed_instructions()
                  << " IRQs=" << hardware.cpu().interrupts().entries() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
