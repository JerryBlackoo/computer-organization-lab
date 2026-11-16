#pragma once

#include <systemc>
#include <cstdint>

namespace colab::simulation::systemc {

// One modeled clock domain; explicit delta phases define inter-module visibility.
class PhaseClock : public sc_core::sc_module {
public:
    sc_core::sc_in<bool> clk{"clk"};
    sc_core::sc_signal<bool> power_good{"power_good"}, reset_asserted{"reset_asserted"};
    sc_core::sc_event bus, components, interrupts, cpu, observe;
    std::uint64_t cycles = 0, remaining = 0;
#if SC_VERSION_MAJOR < 3
    SC_HAS_PROCESS(PhaseClock);
#endif
    explicit PhaseClock(sc_core::sc_module_name name) : sc_core::sc_module(name) { SC_THREAD(run); }
private:
    static void phase(sc_core::sc_event& event) {
        event.notify(sc_core::SC_ZERO_TIME);
        sc_core::wait(sc_core::SC_ZERO_TIME);
        sc_core::wait(sc_core::SC_ZERO_TIME);
    }
    void run() {
        while (true) {
            sc_core::wait(clk.posedge_event());
            if (!power_good.read() || reset_asserted.read() || !remaining) continue;
            // Bus effects commit first. Its b_transport waiters return before
            // the component phase, independent of same-delta process order.
            phase(bus);
            phase(components);
            phase(interrupts);
            phase(cpu);
            ++cycles; --remaining;
            phase(observe);
        }
    }
};

} // namespace colab::simulation::systemc
