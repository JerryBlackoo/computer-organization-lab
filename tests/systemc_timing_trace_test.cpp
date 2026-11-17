#include "../simulation/systemc/platform.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace sc = colab::simulation::systemc;
int sc_main(int argc, char** argv) {
    if (argc != 2) throw std::invalid_argument("VCD output path required");
    colab::SoCConfig config;
    config.enable_timed_dram = true;
    config.instruction_cache_hit_cycles = 2; config.data_cache_hit_cycles = 2;
    sc::BootImage image{0x8000, {0x00300093, 0x00500113, 0x022081b3, 0x06302023, 0x00000063}, {}};
    sc::Platform machine("computer", std::move(image), sc_core::sc_time(50, sc_core::SC_US), config);
    auto* trace = sc_core::sc_create_vcd_trace_file(argv[1]);
    if (!trace) throw std::runtime_error("cannot create VCD");
    machine.trace(trace);
    machine.power_on();
    bool busy = false, finished = false, alu = false, dram = false;
    for (unsigned n = 0; n < 100000 && machine.cpu().completed_instructions() < 4; ++n) {
        machine.step();
        if (!machine.fault().empty()) throw std::runtime_error(machine.fault());
        busy |= machine.mdu_busy.read();
        finished |= machine.mdu_done.read() && machine.mdu_iteration.read() == 32;
        alu |= machine.mdu_alu_used.read();
        dram |= machine.dram_command.read() != 0;
    }
    sc_core::sc_close_vcd_trace_file(trace);
    if (!busy || !finished || !alu || !dram || machine.cpu().registers().read(3) != 15 ||
        machine.cpu().completed_instructions() != 4)
        throw std::runtime_error("timed platform did not expose clocked MDU/DRAM activity");
    std::ifstream input(std::string(argv[1]) + ".vcd");
    const std::string data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    for (const auto* name : {"mdu_busy", "mdu_done", "mdu_partial", "mdu_q", "mdu_alu_used",
                             "walk_busy", "walk_pending", "walk_discarded", "walk_level", "walk_pte_address",
                             "dram_phase", "dram_command", "dram_address"})
        if (data.find(name) == std::string::npos) throw std::runtime_error(std::string("missing VCD signal: ") + name);
    std::cout << "PASS SystemC VCD: clocked MDU, shared ALU, page walker and timed DRAM signals\n";
    return 0;
}
