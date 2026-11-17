#include "../soc/soc.hpp"
#include <iostream>

using namespace colab;
namespace vm = soc::core::mmu;
static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> static void invalid(F action) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid TLB geometry accepted");
}
static vm::PageTableEntry page(unsigned frame = 0x40) {
    return {frame, true, {true, true, true, true}, true, true, 73};
}
static void geometry() {
    vm::TLB legacy(3), defaults;
    require(legacy.geometry().sets() == 1 && legacy.geometry().ways() == 3 &&
        defaults.geometry().capacity() == 16 && defaults.geometry().ways() == 16,
        "legacy capacity constructor/default no longer fully associative");
    vm::TLB three_way(vm::TlbConfig{6, 3});
    require(three_way.geometry().sets() == 2, "non-power-of-two way count rejected");
    for (const vm::TlbConfig config : {vm::TlbConfig{0, 0}, {0, 1}, {16, 0}, {4, 8}, {10, 4}, {12, 4}, {1u << 21, 1}}) {
        invalid([&] { vm::TLB tlb(config); });
        invalid([&] { soc::core::MMU mmu(config); });
        invalid([&] { soc::core::Sv32 sv32(config); });
        invalid([&] { soc::RV32Core cpu({}, soc::RV32Core::Mode::Hardwired, 16, {},
            soc::RV32Core::Execution::MultiCycle, {}, soc::core::FaultHandling::HostException,
            soc::RV32Core::Translation::Teaching, config); });
    }
}
static void replacement(vm::TlbConfig config) {
    vm::TLB tlb(config);
    const auto ways = config.ways, sets = tlb.geometry().sets();
    if (sets > 1) tlb.insert(1, page(0x71));
    for (unsigned i = 0; i < ways; ++i) tlb.insert(static_cast<unsigned>(i * sets), page(0x40 + i));
    require(tlb.lookup(0)->physical_frame == 0x40, "first set fill/hit failed"); // Touch way 0.
    tlb.insert(static_cast<unsigned>(ways * sets), page(0x70));
    const auto evicted = ways == 1 ? 0u : static_cast<unsigned>(sets);
    require(!tlb.lookup(evicted), "replacement is not set-local LRU");
    for (unsigned i = 0; i < ways; ++i) if (i * sets != evicted)
        require(tlb.lookup(static_cast<unsigned>(i * sets))->physical_frame == 0x40 + i, "wrong LRU victim/tag alias");
    require(tlb.lookup(static_cast<unsigned>(ways * sets))->physical_frame == 0x70, "replacement mapping lost");
    if (sets > 1) require(tlb.lookup(1)->physical_frame == 0x71, "replacement evicted another set");
    const auto key = static_cast<unsigned>(ways * sets);
    tlb.insert(key, page(0x72));
    require(tlb.lookup(key)->software_data == 73 && tlb.lookup(key)->physical_frame == 0x72, "same-tag update lost fields");
    tlb.invalidate(key);
    tlb.insert(static_cast<unsigned>((ways + 1) * sets), page(0x73));
    require(!tlb.lookup(key) && tlb.lookup(static_cast<unsigned>((ways + 1) * sets)), "invalid slot reuse failed");
    for (std::size_t i = 0; i < tlb.entries().size(); ++i) {
        const auto& entry = tlb.entries()[i];
        require(!entry.valid || tlb.geometry().set_index(entry.virtual_page) == i / ways, "entry escaped its set");
        require(tlb.lru_rank(i) < ways, "LRU rank is not relative to its set");
    }
    tlb.flush();
    for (const auto& entry : tlb.entries()) require(!entry.valid, "flush left an entry valid");
}
static void translations(vm::TlbConfig config) {
    soc::core::MMU mmu(config);
    unsigned reads = 0;
    mmu.enable_paging(0, [&](unsigned address) { ++reads; return page(address / 8 + 0x40); });
    const auto sets = mmu.tlb().geometry().sets();
    auto load = [&](unsigned vpn) {
        require(mmu.translate(vpn * 4096 + 4) == (vpn + 0x40) * 4096 + 4, "MMU returned wrong PFN/offset");
    };
    load(0); load(0);
    require(reads == 1 && mmu.statistics().tlb_hits == 1, "MMU did not use configured TLB hits");
    if (sets > 1) load(1);
    for (unsigned i = 1; i <= config.ways; ++i) load(static_cast<unsigned>(i * sets));
    const auto before = reads;
    if (sets > 1) { load(1); require(reads == before, "MMU set isolation failed"); }
    load(0); require(reads == before + 1, "conflicting page did not miss/walk after eviction");
    mmu.invalidate_page(0); load(0);
    require(reads == before + 2, "single-page invalidation did not force a walk");
    mmu.disable_paging(); require(mmu.translate(4) == 4, "Bare passthrough changed");
}
static void wired_cpus(vm::TlbConfig config) {
    for (unsigned variant = 0; variant < 3; ++variant) {
        SoCConfig board;
        board.tlb = config;
        board.cpu_execution = variant == 2 ? soc::RV32Core::Execution::Pipelined : soc::RV32Core::Execution::MultiCycle;
        board.cpu_mode = variant == 1 ? soc::RV32Core::Mode::Microprogrammed : soc::RV32Core::Mode::Hardwired;
        SoC system({0x0000a503, 0x0000a583, 0x00000063}, board); // Two actual paged LW instructions.
        system.cpu().set_register(1, 0x5000);
        system.memory().write(0x9000, devices::common::word_bytes(41));
        unsigned reads = 0;
        system.cpu().mmu().enable_paging(0, [&](unsigned address) {
            require(address == 5 * 8, "teaching data translation received wrong VPN");
            ++reads; return page(9);
        });
        for (unsigned n = 0; n < 2000 && system.cpu().completed_instructions() < 2; ++n) system.tick();
        require(system.cpu().completed_instructions() == 2 && system.cpu().registers().read(10) == 41 &&
            system.cpu().registers().read(11) == 41 && reads == 1 && system.cpu().mmu().statistics().tlb_hits == 1,
            "SoC config did not reach actual teaching CPU translation");
        require(system.cpu().sv32().geometry().ways() == config.ways &&
            system.cpu().mmu().tlb().geometry().capacity() == config.entries, "Core config did not reach both TLBs");
    }
    soc::Core old({{isa::Opcode::Add, 3, 1, 2}, {isa::Opcode::Halt}}, config);
    old.set_register(1, 1); old.set_register(2, 2);
    old.mmu().enable_paging(0, [](unsigned) { return page(0); });
    for (unsigned n = 0; n < 128 && !old.is_halted(); ++n) old.tick();
    require(old.is_halted() && old.registers().gprs.read(3) == 3 && old.mmu().statistics().tlb_hits == 1 &&
        old.mmu().tlb().geometry().ways() == config.ways, "16-bit teaching Core TLB configuration failed");
}
int main() {
    geometry();
    for (std::size_t ways : {1u, 2u, 4u, 16u}) {
        const vm::TlbConfig config{16, ways};
        replacement(config); translations(config); wired_cpus(config);
    }
    replacement({6, 3}); translations({8, 2}); wired_cpus({8, 2});
    std::cout << "PASS teaching TLB: geometry, 1/2/4/FA + 3 ways, per-set LRU/isolation, MMU walks, SoC CPU modes and 16-bit Core\n";
}
