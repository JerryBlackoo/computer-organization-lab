#include "io_test_support.hpp"
using namespace io_test;

int main() {
    for (auto mode : {Mode::DmaPolling, Mode::DmaInterrupt}) {
        const auto image = examples::guest::hdd_read(mode);
        SoC system(image.instructions, config()); image.load_constants(system.memory()); seed_hdd(system);
        run_guest(system);
        assert(system.memory().read(0x800, 512) == pattern());
        assert(system.hdd().dma().completed() && system.hdd().dma().transferred() == 512);
        assert(system.cpu().registers().read(9) > 0);
        assert(system.cpu().registers().read(13) == 128); // Guest performed no DATA-word loop.
        assert(system.cpu().registers().read(28) == c::load32(pattern())); // Real guest load sees DMA-written RAM.
        assert(system.cpu().interrupts().entries() == (mode == Mode::DmaInterrupt ? 1u : 0u));
        assert(system.bus().fabric().statistics().posted_writes >= 128);
    }
    SoC system({0x00000063}, config());
    c::DmaEngine dma(system.bus(), 7, 7);
    const auto data = pattern(19);
    dma.start({0x811, 19, c::DmaDirection::DeviceToMemory}, data);
    for (unsigned i = 0; i < 1000 && dma.busy(); ++i) { system.tick(); dma.tick(); }
    assert(dma.completed() && dma.current_address() == 0x824 && dma.remaining() == 0);
    assert(system.memory().read(0x811, 19) == data);
    dma.start({0x811, 19, c::DmaDirection::MemoryToDevice});
    for (unsigned i = 0; i < 1000 && dma.busy(); ++i) { system.tick(); dma.tick(); }
    assert(dma.completed() && dma.data() == data);
    system.iommu().map({7, 0x100000000ULL, 0x900, 512, true, false}); system.iommu().enable();
    dma.start({0x100000000ULL, 4, c::DmaDirection::DeviceToMemory}, {1, 2, 3, 4}); dma.tick();
    assert(dma.failed() && dma.transferred() == 0);
    assert(system.memory().read(0x900, 4) == c::Bytes(4));
    dma.start({0x100000000ULL, 4, c::DmaDirection::MemoryToDevice});
    for (unsigned i = 0; i < 100 && dma.busy(); ++i) { system.tick(); dma.tick(); }
    assert(dma.completed());
    system.iommu().enable(false);
    dma.start({SoC::HddBase, 4, c::DmaDirection::MemoryToDevice}); dma.tick(); assert(dma.failed());
    dma.start({0x100000000ULL, 4, c::DmaDirection::MemoryToDevice}); dma.tick(); assert(dma.failed());
    throws<std::invalid_argument>([&] { dma.start({UINT64_MAX, 4, c::DmaDirection::MemoryToDevice}); });
    std::cout << "PASS DMA: guest polling/IRQ, independent beats, tails, IOMMU and range faults\n";
}
