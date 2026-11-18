#include "io_test_support.hpp"

using namespace io_test;

int main() {
    using Root = soc::controller::PcieRootComplex;
    const auto window = soc::address_map::Hdd;
    const auto address = window.base + Block::Lba;

    examples::guest::Assembler assembler;
    assembler.li(5, window.base);
    assembler.li(6, 7);
    assembler.sw(6, 5, Block::Lba);
    assembler.lw(7, 5, Block::Lba);
    assembler.label("done");
    assembler.beq(0, 0, "done");
    const auto image = assembler.finish();
    SoC system(image.instructions, config());
    image.load_constants(system.memory());

    const auto packet = Root::memory_write(address, {7, 0, 0, 0});
    assert(packet.address == 0x1000000c && packet.data == 7);
    throws<std::invalid_argument>([&] { Root::memory_write(address, {7}); });

    const pcie::BarEndpoint endpoint(window.base, window.size, system.hdd());
    throws<std::out_of_range>([&] { endpoint.receive(pcie::MemoryWrite{address + 0x100, 9}); });
    throws<std::out_of_range>([&] { endpoint.receive(pcie::MemoryRead{window.base - 4}); });
    throws<std::invalid_argument>([&] { endpoint.receive(pcie::MemoryWrite{address + 1, 9}); });
    assert(system.hdd().mmio_read32(Block::Lba) == 0);

    const auto ram_writes = system.memory().statistics().write_requests;
    until(system, [&] { return system.cpu().pc() == image.done_pc; });
    assert(system.hdd().mmio_read32(Block::Lba) == 7 && system.cpu().registers().read(7) == 7);
    assert(system.hdd().mmio_read32(Block::Command) == 0 && system.hdd().mmio_read32(Block::Length) == 0);
    assert(system.memory().statistics().write_requests == ram_writes);
    std::cout << "PASS PCIe MMIO: guest SW -> Memory Write -> BAR -> HDD LBA = 7; LW readback; BAR misses rejected\n";
}
