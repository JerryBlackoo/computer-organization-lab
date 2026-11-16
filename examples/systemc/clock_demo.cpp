#include <systemc>
#include <iostream>

struct TimerModel {
    unsigned ticks = 0;
    bool irq_pending = false;
    void reset() { ticks = 0; irq_pending = false; }
    void tick() { if (++ticks == 3) irq_pending = true; }
};

SC_MODULE(TimerAdapter) {
    sc_core::sc_in<bool> clk{"clk"}, reset{"reset"};
    sc_core::sc_out<bool> irq{"irq"};
    TimerModel hardware;

    void on_clock() {
        if (reset.read()) hardware.reset();
        else hardware.tick();
        irq.write(hardware.irq_pending);
    }
    SC_CTOR(TimerAdapter) {
        SC_METHOD(on_clock);
        sensitive << clk.pos();
        dont_initialize();
    }
};

// An input observer, not the project's RISC-V CPU or its trap handler.
SC_MODULE(CpuObserver) {
    sc_core::sc_in<bool> clk{"clk"}, reset{"reset"}, irq{"irq"};
    bool received_irq = false;
    sc_core::sc_time observed_at = sc_core::SC_ZERO_TIME;

    void on_clock() {
        if (reset.read()) {
            received_irq = false;
            observed_at = sc_core::SC_ZERO_TIME;
        } else if (irq.read() && !received_irq) {
            received_irq = true;
            observed_at = sc_core::sc_time_stamp();
            std::cout << "CPU observes IRQ at " << observed_at << '\n';
        }
    }
    SC_CTOR(CpuObserver) {
        SC_METHOD(on_clock);
        sensitive << clk.pos();
        dont_initialize();
    }
};

int sc_main(int, char**) {
    sc_core::sc_clock clock("clock", 10, sc_core::SC_NS);
    sc_core::sc_signal<bool> reset("reset"), irq("irq");
    TimerAdapter timer("timer");
    CpuObserver cpu("cpu");
    timer.clk(clock); timer.reset(reset); timer.irq(irq);
    cpu.clk(clock); cpu.reset(reset); cpu.irq(irq);

    reset.write(true);
    sc_core::sc_start(15, sc_core::SC_NS);
    reset.write(false);
    sc_core::sc_start(60, sc_core::SC_NS);

    // Counts at 20/30/40ns; IRQ updates after the 40ns evaluation phase.
    // CPU must see it at 50ns, regardless of process evaluation order.
    if (!cpu.received_irq || cpu.observed_at != sc_core::sc_time(50, sc_core::SC_NS)) {
        std::cerr << "FAIL: IRQ was not observed on the expected later edge\n";
        return 1;
    }
    return 0;
}
