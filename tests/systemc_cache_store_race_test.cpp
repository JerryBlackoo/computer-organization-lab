#include "../simulation/systemc/platform.hpp"
#include "../devices/common/mmio_device.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace colab;
namespace sc = simulation::systemc;
namespace c = devices::common;

int sc_main(int argc, char** argv) {
    if (argc != 2) throw std::invalid_argument("expected legacy or interleaved mode");
    const std::string mode = argv[1];
    SoCConfig cfg;
    cfg.memory.rank.dram_chips = 1;
    cfg.memory.rank.dram_chip.banks = 4;
    if (mode == "legacy") cfg.enable_timed_dram = true;
    else if (mode == "interleaved") cfg.dram_mode = DramScheduleMode::InterleavedPipeline;
    else throw std::invalid_argument("unknown DRAM mode");
    cfg.data_cache_hit_cycles = 2;
    sc::BootImage image{0x8000, {0x0000a103, 0x0030a023, 0x0000a203, 0x00000063}, {{0x100, 0}}};
    sc::Platform machine("computer", std::move(image), sc_core::sc_time(50, sc_core::SC_US), cfg);
    machine.power_on();
    machine.cpu().set_register(1, 0x100);
    machine.cpu().set_register(3, 0x11223344);
    bool injected = false;
    c::Tag dma = 0;
    for (unsigned n = 0; n < 10000 && machine.cpu().completed_instructions() < 3; ++n) {
        machine.step();
        if (!machine.fault().empty()) throw std::runtime_error(machine.fault());
        const auto bytes = machine.memory().inspect(0x100, 4);
        if (!injected && bytes[0] == 0x44 && bytes[3] == 0) {
            dma = machine.debug_port().submit(7, c::BusOperation::Write, 0x100, 1, {0xee});
            injected = true;
        }
    }
    if (!injected || !machine.debug_port().ready(dma) ||
        machine.debug_port().take(dma).status != interconnect::pcie::Status::Success ||
        machine.cpu().completed_instructions() != 3 ||
        c::load32(machine.memory().inspect(0x100, 4)) != 0x112233ee ||
        machine.cpu().registers().read(4) != 0x112233ee)
        throw std::runtime_error("DMA/store ordering did not survive the SystemC cache acknowledgement");
    std::cout << "PASS SystemC cache: DMA write between narrow CPU store beats (" << mode << ")\n";
    return 0;
}
