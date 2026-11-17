#include "../simulation/systemc/platform.hpp"
#include "../devices/common/mmio_device.hpp"
#include "../examples/guest/io_driver.hpp"

#include <iostream>
#include <stdexcept>

using namespace colab;
namespace sc = colab::simulation::systemc;
static void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

int sc_main(int argc, char** argv) {
    SoCConfig config;
    if (argc > 1) config.memory.rank.dram_chips = std::stoul(argv[1]);
    config.memory.rank.dram_chip.banks = 4;
    config.dram_mode = DramScheduleMode::InterleavedPipeline;
    examples::guest::Assembler program;
    program.li(1, 0x7000);
    program.lw(2, 1, 0); program.lw(3, 1, 4); program.lw(4, 1, 8); program.lw(5, 1, 12);
    program.sw(5, 0, 0x60); program.label("done"); program.beq(0, 0, "done");
    const auto assembled = program.finish().relocated(0x8000);
    sc::BootImage image{assembled.entry_point, assembled.instructions, assembled.constants};
    image.data.insert(image.data.end(), {{0x7000, 1}, {0x7004, 2}, {0x7008, 3}, {0x700c, 4}});
    sc::Platform machine("computer", std::move(image), sc_core::sc_time(50, sc_core::SC_US), config);
    machine.power_on();
    for (unsigned i = 0; i < 10000 && devices::common::load32(machine.memory().inspect(0x60, 4)) != 4; ++i)
        machine.step();
    require(devices::common::load32(machine.memory().inspect(0x60, 4)) == 4 && machine.fault().empty() &&
            machine.cpu().registers().read(2) == 1 && machine.cpu().registers().read(3) == 2 &&
            machine.cpu().registers().read(4) == 3 && machine.cpu().registers().read(5) == 4,
            "CPU program failed with interleaved DRAM");
    require(machine.interleaved_dram() && machine.interleaved_dram()->scheduler().statistics().accesses > 0 &&
            machine.dram_timing().cycles() == 0, "wrong DRAM scheduler ticked");
    auto tag = machine.debug_port().submit(0, devices::common::BusOperation::Write, 0x7000, 4,
                                            devices::common::word_bytes(0x12345678));
    for (unsigned i = 0; i < 10000 && !machine.debug_port().ready(tag); ++i) machine.step();
    require(machine.debug_port().ready(tag) && machine.debug_port().take(tag).status == interconnect::pcie::Status::Success,
            "timed debug write failed");
    tag = machine.debug_port().submit(0, devices::common::BusOperation::Read, 0x7000, 4);
    for (unsigned i = 0; i < 10000 && !machine.debug_port().ready(tag); ++i) machine.step();
    require(machine.debug_port().ready(tag), "timed debug read did not finish");
    auto read = machine.debug_port().take(tag);
    require(read.status == interconnect::pcie::Status::Success && devices::common::load32(read.data) == 0x12345678,
            "timed debug read returned wrong value");
    // Reset while work is pending; RAM is preserved and new transactions complete.
    tag = machine.debug_port().submit(0, devices::common::BusOperation::Read, 0x7000, 4);
    for (unsigned i = 0; i < 1000 && !machine.interleaved_dram()->busy(); ++i) machine.step();
    require(machine.interleaved_dram()->busy(), "no live request to reset");
    machine.reset();
    require(!machine.interleaved_dram()->busy() && devices::common::load32(machine.memory().inspect(0x7000, 4)) == 0x12345678,
            "reset failed to cancel request or preserve RAM");
    tag = machine.debug_port().submit(0, devices::common::BusOperation::Read, 0x7000, 4);
    for (unsigned i = 0; i < 10000 && !machine.debug_port().ready(tag); ++i) machine.step();
    require(machine.debug_port().ready(tag), "read after reset did not finish");
    read = machine.debug_port().take(tag);
    require(read.status == interconnect::pcie::Status::Success && devices::common::load32(read.data) == 0x12345678,
            "read after reset lost retained RAM");
    std::cout << "PASS SystemC interleaved DRAM: CPU, debug port and live reset\n";
    return 0;
}
