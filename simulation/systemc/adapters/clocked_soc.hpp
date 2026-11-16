#pragma once

#include "../../../soc/soc.hpp"
#include <systemc>

namespace colab::simulation::systemc {

// First migration step: the SystemC kernel clocks the existing complete SoC.
// Keep SoC::tick's ordering until shared objects have explicit channel boundaries.
class ClockedSoC : public sc_core::sc_module {
public:
    sc_core::sc_in<bool> clk{"clk"};
    sc_core::sc_signal<std::uint64_t> cycles{"cycles"};
    sc_core::sc_signal<std::uint32_t> pc{"pc"};
    sc_core::sc_signal<std::uint64_t> retired{"retired"};
    sc_core::sc_signal<bool> irq_pending{"irq_pending"};
    sc_core::sc_signal<std::uint64_t> interrupt_entries{"interrupt_entries"};
    sc_core::sc_signal<std::uint64_t> nvme_commands{"nvme_commands"};
    sc_core::sc_signal<std::uint64_t> hdd_dma_bytes{"hdd_dma_bytes"};

#if SC_VERSION_MAJOR < 3
    SC_HAS_PROCESS(ClockedSoC);
#endif
    ClockedSoC(sc_core::sc_module_name name, std::vector<std::uint32_t> program,
               SoCConfig config = {})
        : sc_core::sc_module(name), hardware_(std::move(program), config) {
        SC_METHOD(on_clock);
        sensitive << clk.pos();
        dont_initialize();
    }

    // Host initialization/inspection only. Do not tick this object separately.
    SoC& hardware() { return hardware_; }

    void trace(sc_core::sc_trace_file* file) const {
        sc_core::sc_trace(file, clk, "clock");
        sc_core::sc_trace(file, cycles, "cycles");
        sc_core::sc_trace(file, pc, "pc");
        sc_core::sc_trace(file, retired, "retired");
        sc_core::sc_trace(file, irq_pending, "irq_pending");
        sc_core::sc_trace(file, interrupt_entries, "interrupt_entries");
        sc_core::sc_trace(file, nvme_commands, "nvme_commands");
        sc_core::sc_trace(file, hdd_dma_bytes, "hdd_dma_bytes");
    }

private:
    void on_clock() {
        hardware_.tick();
        // sc_signal publishes these snapshots in the kernel's update phase.
        cycles.write(hardware_.cycles());
        pc.write(hardware_.cpu().pc());
        retired.write(hardware_.cpu().completed_instructions());
        irq_pending.write(hardware_.interrupts().requested());
        interrupt_entries.write(hardware_.cpu().interrupts().entries());
        nvme_commands.write(hardware_.nvme().controller().completed_commands());
        hdd_dma_bytes.write(hardware_.hdd().dma().transferred());
    }

    SoC hardware_;
};

} // namespace colab::simulation::systemc
