#include "io_test_support.hpp"
using namespace io_test;

int main() {
    examples::guest::Assembler a;
    a.label("done"); a.lw(8, 0, 0x60); a.addi(8, 8, 1); a.sw(8, 0, 0x60); a.beq(0, 0, "done");
    SoC system(a.finish().instructions, config()); seed_hdd(system); create_io_queue(system);
    system.nvme().nand().write(0, pattern()); system.memory().write(0x7000, pattern(128));
    write(system, SoC::NicBase + devices::NIC::TxAddress, 0x7000);
    write(system, SoC::NicBase + devices::NIC::RxAddress, 0x7800);
    write(system, SoC::NicBase + devices::NIC::Length, 128);
    write(system, SoC::HddBase + Block::Length, 512);
    write(system, SoC::HddBase + Block::DmaAddress, 0x6000);
    write(system, SoC::HddBase + Block::Control, Block::EnableDma);
    system.memory().write(0x3000, command(2, 10, 0x5000, 1));
    write(system, SoC::HddBase + Block::Command, Block::Read);
    write(system, SoC::NvmeBase + 0x1008, 1);
    write(system, SoC::NicBase + devices::NIC::Command, 1);
    const auto cpu_read = system.bus().submit(0, c::BusOperation::Read, 0x7000, 4);
    std::set<unsigned> masters;
    until(system, [&] {
        if (system.bus().fabric().last_tick().master) masters.insert(*system.bus().fabric().last_tick().master);
        return system.hdd().state() == devices::hdd::DeviceState::Completed &&
            system.nvme().controller().completed_commands() == 3 &&
            system.nic().mmio_read32(devices::NIC::Status) == 2 && system.bus().ready(cpu_read);
    });
    const auto expected = pattern();
    assert(system.bus().take(cpu_read).data == c::Bytes(expected.begin(), expected.begin() + 4));
    assert(system.memory().read(0x5000, 512) == pattern());
    assert(system.memory().read(0x6000, 512) == pattern());
    assert(system.memory().read(0x7800, 128) == pattern(128));
    assert(masters.count(SoC::HddMaster) && masters.count(SoC::NvmeMaster) && masters.count(SoC::NicMaster));
    assert(masters.count(0));
    assert(c::load32(system.memory().read(0x60, 4)) > 0);
    assert(system.cpu().completed_instructions() > 0);
    std::cout << "PASS integration: CPU + HDD + NVMe + NIC share one timed fabric and DIMM\n";
}
