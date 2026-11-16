#include "../simulation/systemc/platform.hpp"
#include "../examples/guest/io_driver.hpp"
#include "elf_fixture.hpp"
#include <iostream>
using namespace colab;
namespace sc = simulation::systemc;
namespace c = devices::common;
static void require(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
int sc_main(int, char**) {
    try {
        constexpr std::uint32_t capacity = 0x40000, entry = 0x30000;
        require(SoCConfig{}.memory.capacity_bytes() == capacity, "default board capacity mismatch");
        examples::guest::Assembler a;
        for (unsigned rank = 0; rank < 4; ++rank) {
            a.li(1, rank * 0x10000 + 0x400); a.lw(2, 1, 0); a.sw(2, 0, 0x60 + rank * 4);
            a.addi(2, 2, 1); a.sw(2, 1, 4);
        }
        a.li(1, capacity - 4); a.li(2, 0x12345678); a.sw(2, 1, 0); a.label("done"); a.emit(0x10500073); // WFI
        auto guest = a.finish();
        const auto& code = guest.instructions;
        require(code.size() * 4 <= 0x100, "ELF fixture text overlaps data file offset");
        auto bytes = elf_fixture();
        c::store32(bytes, 24, entry); c::store32(bytes, 60, entry); c::store32(bytes, 64, entry);
        c::store32(bytes, 68, code.size() * 4); c::store32(bytes, 72, code.size() * 4);
        for (unsigned i = 0; i < code.size(); ++i) c::store32(bytes, 0x100 + i * 4, code[i]);
        c::store32(bytes, 92, capacity - 16); c::store32(bytes, 96, capacity - 16);
        // Assembler LI uses real LW instructions from a low-RAM constant pool.
        // Include that pool as a third ELF PT_LOAD segment, not host execution.
        const auto pool_size = static_cast<std::uint32_t>(guest.constants.size() * 4);
        bytes.resize(0x240 + pool_size); bytes[44] = 3;
        c::store32(bytes, 116, 1); c::store32(bytes, 120, 0x240);
        c::store32(bytes, 124, 0x100); c::store32(bytes, 128, 0x100);
        c::store32(bytes, 132, pool_size); c::store32(bytes, 136, pool_size);
        c::store32(bytes, 140, 4); c::store32(bytes, 144, 4);
        for (const auto& constant : guest.constants)
            c::store32(bytes, 0x240 + constant.first - 0x100, constant.second);
        auto elf = simulation::boot::parse_elf32(bytes, capacity);
        bool too_large = false;
        try { (void)simulation::boot::parse_elf32(bytes, capacity - 1); }
        catch (const std::exception&) { too_large = true; }
        require(too_large, "ELF BSS end limit ignored");
        sc::BootImage boot; boot.entry = elf.entry; boot.segments = std::move(elf.segments);
        sc::Platform m("four_rank_machine", std::move(boot));
        auto word = [&](std::uint32_t at) { return c::load32(m.memory().read(at, 4)); };
        auto seed = [&] {
            for (unsigned rank = 0; rank < 4; ++rank)
                m.memory().write(rank * 0x10000 + 0x400, c::word_bytes(0xaabbcc00 + rank));
        };
        auto execute = [&] {
            for (unsigned i = 0; i < 10000 && !m.cpu().waiting(); ++i) m.step();
            require(m.cpu().waiting() && m.fault().empty() && m.cpu().interrupts().exceptions() == 0,
                "upper-rank ELF did not finish through Cache/TLM/DIMM");
            for (unsigned rank = 0; rank < 4; ++rank)
                require(word(0x60 + rank * 4) == 0xaabbcc00 + rank &&
                    word(rank * 0x10000 + 0x404) == 0xaabbcc01 + rank, "CPU rank data aliased");
            require(word(capacity - 4) == 0x12345678 && m.cpu().pc() >= entry &&
                m.instruction_cache().statistics().bypasses == 0 && m.data_cache().statistics().bypasses == 0,
                "high RAM was not cached/last word failed");
        };
        m.power_on();
        require(word(capacity - 16) == 0xcafebabe && word(capacity - 4) == 0,
            "Rank3 ELF initialized data/BSS loading failed");
        require(m.bus().is_memory(0, capacity) && !m.bus().is_memory(capacity, 1), "SystemC RAM map stale");
        seed(); execute();
        m.reset();
        for (unsigned rank = 0; rank < 4; ++rank)
            require(word(rank * 0x10000 + 0x404) == 0xaabbcc01 + rank, "warm reset lost upper RAM");
        execute();
        m.power_off(); sc_core::sc_start(sc_core::sc_time(2, sc_core::SC_MS));
        require(!m.powered() && word(capacity - 4) == 0x12345678, "power-off changed retained RAM");
        m.power_on();
        for (unsigned rank = 0; rank < 4; ++rank)
            require(word(rank * 0x10000 + 0x404) == 0, "cold power-on failed to clear all ranks");
        require(word(capacity - 4) == 0 && word(capacity - 16) == 0xcafebabe,
            "cold power-on did not reload ELF/BSS at capacity end");
        seed(); execute();
        // Real translated fetch: root in Rank1, L2 in Rank2, executable leaf in Rank3.
        m.reset(); m.memory().write(entry, c::word_bytes(0x30200073)); // MRET
        m.memory().write(0x10400, c::word_bytes((0x20u << 10) | 1));
        m.memory().write(0x20030, c::word_bytes((0x31u << 10) | 0x4b)); // S RXA
        m.memory().write(0x31000, c::word_bytes(0x04900513)); // ADDI a0,zero,73
        m.memory().write(0x31004, c::word_bytes(0x10500073));
        auto& state = m.cpu().interrupts();
        state.write(0x3b0, 0x1fffffff); state.write(0x3a0, 0x1f);
        state.write(0x180, 0x80000010); state.write(0x341, 0x4000c000); state.write(0x300, 1u << 11);
        for (unsigned i = 0; i < 10000 && !m.cpu().waiting(); ++i) m.step();
        require(m.cpu().waiting() && m.cpu().registers().read(10) == 73 &&
            m.cpu().sv32().statistics().reads >= 2 && !state.exceptions() && m.fault().empty(),
            "Sv32 upper-rank root/L2/leaf path failed");
        std::cout << "PASS SystemC four-rank: Rank3 ELF fetch, all-rank CPU cached loads/stores, "
            "last word, ELF/BSS limit, warm reset retains all ranks, cold clear/reload after power-off, "
            "Sv32 root Rank1 / L2 Rank2 / leaf Rank3\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return 0;
}
