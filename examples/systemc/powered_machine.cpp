#include "../../soc/controller/memory/memory.hpp"
#include "../../devices/common/mmio_device.hpp"
#include <systemc>
#include <iostream>
#include <memory>

using namespace sc_core;
using sc_dt::sc_uint;
namespace common = colab::devices::common;

SC_MODULE(Power) {
    sc_out<bool> power_good{"power_good"}, reset{"reset"};
    void run() {
        power_good.write(false); reset.write(true);
        wait(20, SC_NS);
        power_good.write(true);
        wait(20, SC_NS);
        reset.write(false);
    }
    SC_CTOR(Power) { SC_THREAD(run); }
};

// Demonstration sequencer only: pc+=4 is not RISC-V instruction execution.
SC_MODULE(DemoCpu) {
    sc_in<bool> clk{"clk"}, power_good{"power_good"}, reset{"reset"}, irq{"irq"};
    sc_out<bool> dma_start{"dma_start"};
    std::uint32_t pc = 0;
    bool started = false, irq_handled = false;
    sc_time observed_at = SC_ZERO_TIME;
    void run() {
        dma_start.write(false);
        while (true) {
            wait(clk.posedge_event());
            if (!power_good.read() || reset.read()) {
                pc = 0; started = false; irq_handled = false; observed_at = SC_ZERO_TIME;
                dma_start.write(false);
                continue;
            }
            pc += 4;
            dma_start.write(!started);
            if (!started) {
                started = true;
                std::cout << sc_time_stamp() << " CPU: start DMA\n";
            }
            if (irq.read() && !irq_handled) {
                irq_handled = true; observed_at = sc_time_stamp();
                std::cout << observed_at << " CPU: DMA interrupt, PC=" << pc << '\n';
            }
        }
    }
    SC_CTOR(DemoCpu) { SC_THREAD(run); }
};

// A one-write handshake demo, separate from the existing bus-master DMA engine.
SC_MODULE(DemoDma) {
    sc_in<bool> clk{"clk"}, power_good{"power_good"}, reset{"reset"};
    sc_in<bool> start{"start"}, ack{"ack"}, error{"error"};
    sc_out<bool> req{"req"}, irq{"irq"};
    sc_out<sc_uint<32>> addr{"addr"}, data{"data"};
    enum class State { Idle, WaitAck, Done, Error };
    State state = State::Idle;
    void run() {
        req.write(false); irq.write(false); addr.write(0); data.write(0);
        while (true) {
            wait(clk.posedge_event());
            if (!power_good.read() || reset.read()) {
                state = State::Idle;
                req.write(false); irq.write(false); addr.write(0); data.write(0);
            } else if (state == State::Idle && start.read()) {
                addr.write(0x20); data.write(0xCAFE1234); req.write(true);
                state = State::WaitAck;
                std::cout << sc_time_stamp() << " DMA: write request\n";
            } else if (state == State::WaitAck && ack.read()) {
                req.write(false); irq.write(true);
                state = error.read() ? State::Error : State::Done;
                std::cout << sc_time_stamp() << " DMA: " << (error.read() ? "failed" : "completed") << '\n';
            }
        }
    }
    SC_CTOR(DemoDma) { SC_THREAD(run); }
};

class DramEndpoint : public sc_module {
public:
    sc_in<bool> clk{"clk"}, power_good{"power_good"}, reset{"reset"}, req{"req"};
    sc_in<sc_uint<32>> addr{"addr"}, data{"data"};
    sc_out<bool> ack{"ack"}, error{"error"};
    unsigned writes = 0;
#if SC_VERSION_MAJOR < 3
    SC_HAS_PROCESS(DramEndpoint);
#endif
    DramEndpoint(sc_module_name name, colab::soc::controller::Memory& memory)
        : sc_module(name), memory_(memory) {
        SC_METHOD(step);
        sensitive << clk.pos();
        dont_initialize();
    }
private:
    void step() {
        if (!power_good.read() || reset.read() || !req.read()) {
            accepted_ = false; ack.write(false); error.write(false);
            return; // Reset handshake state, not the retained memory contents.
        }
        if (accepted_) return;
        const auto address = addr.read().to_uint();
        try {
            if (address % 4) throw std::out_of_range("unaligned demo DMA write");
            memory_.write(address, common::word_bytes(data.read().to_uint()));
            ++writes;
            std::cout << sc_time_stamp() << " DRAM: data written\n";
        } catch (const std::out_of_range&) { error.write(true); }
        accepted_ = true;
        ack.write(true); // Hold ACK until REQ drops; never accept the same write twice.
    }
    colab::soc::controller::Memory& memory_;
    bool accepted_ = false;
};

int sc_main(int argc, char** argv) {
    if (argc > 2) {
        std::cerr << "Usage: " << argv[0] << " [VCD-path-without-extension]\n";
        return 2;
    }
    sc_clock clock("clock", 10, SC_NS);
    sc_signal<bool> power_good("power_good"), reset("reset"), start("start");
    sc_signal<bool> req("req"), ack("ack"), irq("irq"), error("error");
    sc_signal<sc_uint<32>> addr("addr"), data("data");
    colab::dimm::DIMM dimm({4, {4, {2, {64, 128, 8}}}});
    colab::soc::controller::Memory memory(dimm);
    Power power("power");
    DemoCpu cpu("cpu");
    DemoDma dma("dma");
    DramEndpoint dram("dram", memory);
    power.power_good(power_good); power.reset(reset);
    cpu.clk(clock); cpu.power_good(power_good); cpu.reset(reset); cpu.irq(irq); cpu.dma_start(start);
    dma.clk(clock); dma.power_good(power_good); dma.reset(reset); dma.start(start);
    dma.ack(ack); dma.error(error); dma.req(req); dma.irq(irq); dma.addr(addr); dma.data(data);
    dram.clk(clock); dram.power_good(power_good); dram.reset(reset); dram.req(req);
    dram.ack(ack); dram.error(error); dram.addr(addr); dram.data(data);

    std::unique_ptr<sc_trace_file, decltype(&sc_close_vcd_trace_file)> trace(nullptr, sc_close_vcd_trace_file);
    if (argc == 2) {
        trace.reset(sc_create_vcd_trace_file(argv[1]));
        if (!trace) return 1;
        trace->set_time_unit(1, SC_NS);
        sc_trace(trace.get(), clock, "clock"); sc_trace(trace.get(), power_good, "power_good");
        sc_trace(trace.get(), reset, "reset"); sc_trace(trace.get(), start, "dma_start");
        sc_trace(trace.get(), req, "dma_request"); sc_trace(trace.get(), ack, "dram_ack");
        sc_trace(trace.get(), irq, "interrupt"); sc_trace(trace.get(), error, "error");
        sc_trace(trace.get(), addr, "address"); sc_trace(trace.get(), data, "data");
    }
    sc_start(35, SC_NS);
    if (cpu.pc || dram.writes || common::load32(memory.read(0x20, 4))) {
        std::cerr << "FAIL: hardware progressed before reset release\n";
        return 1;
    }
    sc_start(115, SC_NS);
    const auto value = common::load32(memory.read(0x20, 4));
    std::cout << std::hex << "DRAM[0x20] = 0x" << value << '\n';
    if (value != 0xCAFE1234 || dram.writes != 1 || dma.state != DemoDma::State::Done ||
        !cpu.irq_handled || cpu.observed_at != sc_time(80, SC_NS) || cpu.pc <= 4 || req.read() || ack.read()) {
        std::cerr << "FAIL: power/reset/DMA/IRQ handshake verification failed\n";
        return 1;
    }
    return 0;
}
