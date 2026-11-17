#include "../../soc/controller/memory/interleaved_dram_memory.hpp"
#include "../../devices/common/mmio_device.hpp"

#include <iostream>

int main() {
    using namespace colab;
    namespace p = interconnect::pcie;
    dimm::DIMM module({1, {4, {4, {2, 4, 8}}}});
    soc::controller::Memory memory(module);
    soc::controller::InterleavedDramMemory timing(memory, 0, {1, 4, true, 0});
    for (unsigned i = 0; i < 4; ++i) timing.prepare(i, p::Operation::Read, i * 4, 4);
    std::cout << "tick  command   completed word\n";
    unsigned completed = 0;
    while (completed < 4) {
        timing.tick();
        const auto trace = timing.scheduler().last_tick();
        std::cout << trace.cycle << "     ";
        if (trace.command == soc::controller::InterleavedDramScheduler::Command::Activate)
            std::cout << "ACT bank " << trace.address.bank;
        else std::cout << "          ";
        for (unsigned i = completed; i < 4; ++i) {
            if (!timing.prepare(i, p::Operation::Read, i * 4, 4)) continue;
            (void)memory.read(i * 4, 4);
            timing.complete(i, i * 4);
            ++completed;
            std::cout << "  word " << i;
        }
        std::cout << '\n';
    }
    std::cout << "Measured " << timing.scheduler().cycles() << " ticks; Ta + (n-1)T = 4 + 3*1 = 7 ticks\n";
}
