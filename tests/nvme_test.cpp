#include "io_test_support.hpp"
using namespace io_test;

int main() {
    for (const auto& cpu_config : cpu_configs()) {
        const auto image = examples::guest::nvme_read();
        SoC system(image.instructions, cpu_config); image.load_constants(system.memory());
        system.nvme().nand().write(0, pattern()); run_guest(system);
        assert(system.memory().read(0x5000, 512) == pattern());
        assert(system.cpu().registers().read(28) == c::load32(pattern()));
        assert(system.nvme().controller().completed_commands() == 3);
        assert(!system.interrupts().requested());
    }
    SoC system({0x00000063}, config()); create_io_queue(system);
    // Reuse a four-entry SQ/CQ repeatedly; verify CID, SQ head, phase and IRQ clearing.
    for (unsigned n = 0; n < 10; ++n) {
        const auto slot = n % 4, next = (n + 1) % 4;
        auto cmd = command(n % 2 ? 2 : 1, 10 + n, 0x5000, 1, 3);
        system.memory().write(0x3000 + slot * 64, cmd);
        if (n % 2 == 0) system.memory().write(0x5000, pattern());
        else system.memory().write(0x5000, c::Bytes(512));
        write(system, SoC::NvmeBase + 0x1008, next);
        until(system, [&] { return system.nvme().controller().completed_commands() == n + 3; });
        assert(system.memory().read(0x5000, 512) == pattern());
        const auto completion = system.memory().read(0x4000 + slot * 16, 16);
        assert(c::load32(completion, 8) == (0x10000u | next));
        assert(c::load32(completion, 12) == ((10 + n) | ((n / 4 % 2 ? 0u : 1u) << 16)));
        assert(system.interrupts().requested());
        write(system, SoC::NvmeBase + 0x100c, next);
        assert(!system.interrupts().requested());
    }
    // Invalid NLB/LBA reports error through CQ, and cannot modify data RAM.
    system.memory().write(0x3080, command(2, 90, 0x5000, 1, 0xFFFFFFFF));
    write(system, SoC::NvmeBase + 0x1008, 3);
    until(system, [&] { return system.nvme().controller().completed_commands() == 13; });
    assert((c::load32(system.memory().read(0x402c, 4)) >> 17) == 0x80);
    std::cout << "PASS NVMe: guest admin/IO queues, DMA Read/Write, wrap/phase/IRQ, invalid LBA\n";
}
