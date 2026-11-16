#include "../soc/soc.hpp"
#include <iostream>

using namespace colab;
namespace c = devices::common;
static void require(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
template<class F> static void outside(F f) {
    bool rejected = false;
    try { f(); } catch (const std::out_of_range&) { rejected = true; }
    require(rejected, "out-of-range request accepted");
}
int main() {
    try {
        require(dimm::DIMMGeometry{}.ranks == 4, "standalone DIMM default is not four ranks");
        SoC system;
        auto& memory = system.memory(); auto& module = system.dimm();
        const auto& g = module.geometry();
        constexpr std::uint32_t rank_bytes = 0x10000, capacity = 0x40000;
        require(g.ranks == 4 && g.word_bits() == 32 && g.rank.dram_chips == 4 &&
            g.rank.dram_chip.banks == 2 && g.rank.dram_chip.bank.rows == 64 &&
            g.rank.dram_chip.bank.columns == 128 && g.rank.dram_chip.bank.data_bits == 8 &&
            g.rank.capacity_bytes() == rank_bytes && g.capacity_bytes() == capacity,
            "board geometry changed beyond rank count");
        require(system.bus().is_memory(0, capacity) && system.bus().is_memory(capacity - 1, 1) &&
            !system.bus().is_memory(capacity, 1), "SoC bus RAM window did not follow geometry");
        c::Bytes reference(capacity);
        for (std::uint32_t a = 0; a < capacity; ++a)
            reference[a] = static_cast<std::uint8_t>(a ^ (a >> 8) ^ ((a >> 16) * 53));
        memory.write(0, reference);
        for (std::uint32_t w = 0; w < capacity / 4; ++w) {
            const auto d = module.decode(w);
            require(d.rank == w / 16384 && d.bank == (w & 1) && d.row == ((w >> 8) & 63) &&
                d.column == ((w >> 1) & 127) && module.encode(d) == w, "rank/bank/row/column mapping changed");
            const auto byte = memory.decode(w * 4 + 1);
            require(byte.byte_in_word == 1 && byte.word_address == w && byte.dram.rank == d.rank,
                "byte-to-word conversion changed");
            require(module.read(w) == c::load32(reference, w * 4), "rank alias/data corruption");
        }
        require(memory.read(0, capacity) == reference, "full four-rank flat-reference mismatch");
        auto patch = [&](std::uint32_t a, c::Bytes bytes) {
            memory.write(a, bytes); std::copy(bytes.begin(), bytes.end(), reference.begin() + a);
            require(memory.read(0, capacity) == reference, "partial/cross-rank write damaged neighbors");
        };
        for (unsigned rank = 0; rank < 4; ++rank) {
            const auto base = rank * rank_bytes;
            patch(base, c::word_bytes(0x12345670 + rank));
            patch(base + rank_bytes - 1, {static_cast<std::uint8_t>(0xa0 + rank)});
            const auto d = memory.decode(base + 1045);
            require(d.dram.rank == rank && d.dram.bank == 1 && d.dram.row == 1 &&
                d.dram.column == 2 && d.byte_in_word == 1, "1045 local coordinate changed");
        }
        for (unsigned rank = 1; rank < 4; ++rank)
            patch(rank * rank_bytes - 3, {1, 2, 3, 4, 5, 6, 7, 8, 9});
        const auto end = memory.read(capacity - 4, 4), start = memory.read(0, 4);
        outside([&] { memory.read(capacity, 1); });
        outside([&] { memory.read(capacity - 1, 2); });
        outside([&] { memory.write(capacity - 1, {0, 0}); });
        outside([&] { module.read(capacity / 4); });
        require(memory.read(capacity - 4, 4) == end && memory.read(0, 4) == start,
            "rejected transfer wrapped or partially committed");
        // Real queued SoC fabric, not direct controller calls, in both DMA directions.
        c::DmaEngine dma(system.bus(), 7, 16);
        auto finish = [&] {
            for (unsigned i = 0; i < 10000 && dma.busy(); ++i) { system.bus().tick(); dma.tick(); }
            require(dma.completed() && dma.transferred() == 16, "high/cross-rank DMA failed");
        };
        for (const std::uint32_t a : {0x500u, 0x10500u, 0x20500u, 0x30500u, 0xfffcu, 0x1fffcu, 0x2fffcu, 0x3fff0u}) {
            c::Bytes payload(16);
            for (unsigned i = 0; i < 16; ++i) payload[i] = static_cast<std::uint8_t>((a >> 8) + i);
            dma.start({a, 16, c::DmaDirection::DeviceToMemory}, payload); finish();
            dma.start({a, 16, c::DmaDirection::MemoryToDevice}); finish();
            require(dma.data() == payload && memory.read(a, 16) == payload, "DMA data mismatch");
        }
        dma.start({capacity, 16, c::DmaDirection::DeviceToMemory}, c::Bytes(16, 0xff));
        dma.tick(); require(dma.failed() && dma.transferred() == 0, "out-of-range DMA was not rejected");
        require(memory.read(0, 4) == start, "out-of-range DMA aliased rank zero");
        SoCConfig custom; custom.memory.ranks = 1;
        SoC single({0x00000063}, custom);
        require(single.dimm().geometry().ranks == 1 && !single.bus().is_memory(rank_bytes, 1),
            "explicit single-rank configuration lost");
        std::cout << "PASS four-rank board: 65536 words / 262144 bytes flat reference, all coordinates, independent ranks, "
            "every boundary + partial RMW, last byte/overflow rejection, 16 queued DMA transfers + overflow, explicit one-rank override\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
