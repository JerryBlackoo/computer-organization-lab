#include "../soc/controller/memory/interleaved_dram_memory.hpp"
#include "../devices/common/mmio_device.hpp"
#include "../soc/soc.hpp"

#include <iostream>
#include <stdexcept>
#include <vector>

using namespace colab;
namespace p = interconnect::pcie;
using Timed = soc::controller::InterleavedDramMemory;
using Scheduler = soc::controller::InterleavedDramScheduler;
static void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

static void concurrent_narrow_store() {
    namespace c = devices::common;
    for (unsigned chips : {1u, 3u}) for (unsigned mode : {1u, 2u}) for (unsigned hit_cycles : {1u, 2u})
        for (unsigned cpu_mode : {0u, 1u, 2u}) {
        SoCConfig cfg;
        cfg.memory.rank.dram_chips = chips;
        cfg.memory.rank.dram_chip.banks = 4;
        cfg.cpu_execution = cpu_mode == 2 ? soc::RV32Core::Execution::Pipelined : soc::RV32Core::Execution::MultiCycle;
        cfg.cpu_mode = cpu_mode == 1 ? soc::RV32Core::Mode::Microprogrammed : soc::RV32Core::Mode::Hardwired;
        cfg.data_cache_hit_cycles = hit_cycles;
        cfg.enable_timed_dram = mode == 1;
        if (mode == 2) cfg.dram_mode = DramScheduleMode::InterleavedPipeline;
        // Warm the line, write it in several beats, then read the same word.
        SoC machine({0x0000a103, 0x0030a023, 0x0000a203, 0x00000063}, cfg);
        machine.cpu().set_register(1, 0x100);
        machine.cpu().set_register(3, 0x11223344);
        machine.memory().write(0x100, c::word_bytes(0));
        bool injected = false;
        c::Tag dma = 0;
        for (unsigned n = 0; n < 10000 && machine.cpu().completed_instructions() < 3; ++n) {
            machine.tick();
            const auto bytes = machine.memory().inspect(0x100, 4);
            if (!injected && bytes[0] == 0x44 && bytes[3] == 0) {
                dma = machine.bus().submit(7, p::Operation::Write, 0x100, 1, {0xee});
                injected = true;
            }
        }
        require(injected && machine.bus().ready(dma) && machine.bus().take(dma).status == p::Status::Success &&
                machine.cpu().completed_instructions() == 3, "interleaved store fixture did not complete");
        require(c::load32(machine.memory().inspect(0x100, 4)) == 0x112233ee &&
                machine.cpu().registers().read(4) == 0x112233ee,
                "CPU store acknowledgement overwrote a later DMA byte in cache");
    }
}

int main() {
    concurrent_narrow_store();
    for (unsigned chips : {1u, 2u, 3u, 4u}) for (unsigned mode : {0u, 1u, 2u}) {
        SoCConfig board;
        board.memory.rank.dram_chips = chips;
        board.memory.rank.dram_chip.banks = 4;
        board.cpu_execution = soc::RV32Core::Execution::MultiCycle;
        board.enable_timed_dram = mode == 1;
        if (mode == 2) board.dram_mode = DramScheduleMode::InterleavedPipeline;
        SoC machine({0x0000a103}, board); // LW x2, 0(x1)
        machine.cpu().set_register(1, 0x100);
        machine.memory().write(0x100, devices::common::word_bytes(42));
        for (unsigned n = 0; n < 10000 && !machine.cpu().completed_instructions(); ++n) machine.tick();
        require(machine.cpu().completed_instructions() == 1 && machine.cpu().registers().read(2) == 42,
                "narrow rank failed to serve a CPU word load");
    }
    constexpr std::uint32_t base = 0x1000;
    dimm::DIMM module({1, {4, {4, {2, 4, 8}}}});
    soc::controller::Memory memory(module, base);
    for (std::uint32_t i = 0; i < 4; ++i) {
        memory.write(base + i * 4, devices::common::word_bytes(0x12340000 + i));
        memory.precharge(base + i * 4);
    }
    Timed timed(memory, base, {1, 4, true, 2});
    for (std::uint32_t i = 0; i < 4; ++i)
        require(!timed.prepare(i + 1, p::Operation::Read, base + 4 * i, 4), "new beat was ready");
    for (unsigned cycle = 1; cycle <= 7; ++cycle) {
        timed.tick();
        const auto& trace = timed.scheduler().last_tick();
        if (cycle <= 4) {
            require(trace.command == Scheduler::Command::Activate && trace.address.bank == cycle - 1,
                    "incorrect ACT schedule");
            require(memory.active_row(base + 4 * (cycle - 1)) == 0, "ACT did not reach DIMM");
        }
        const auto before = module.rank(0).dram_chip(0).bank(0).stats();
        (void)memory.inspect(base + 64, 4);
        const auto after = module.rank(0).dram_chip(0).bank(0).stats();
        require(before.activations == after.activations && before.precharges == after.precharges && before.reads == after.reads,
                "inspect issued commands");
        if (cycle >= 4) {
            const auto i = cycle - 4;
            require(timed.prepare(i + 1, p::Operation::Read, base + 4 * i, 4), "late data");
            require(devices::common::load32(memory.read(base + 4 * i, 4)) == 0x12340000 + i, "bad data");
            timed.complete(i + 1, base + 4 * i);
        }
    }
    require(!timed.busy(), "unfinished pipeline");
    std::cout << "Real DIMM: four words completed in " << timed.scheduler().cycles() << " ticks; formula 4 + 3 * 1 = 7\n";

    // An already-open row, including one retained across reset, must be observed.
    timed.reset();
    require(!timed.prepare(20, p::Operation::Read, base + 64, 4), "new row beat was ready");
    timed.tick();
    require(timed.scheduler().last_tick().command == Scheduler::Command::Precharge && !memory.active_row(base),
            "existing row was not precharged");
    timed.tick(); timed.tick();
    require(timed.scheduler().last_tick().command == Scheduler::Command::Activate && memory.active_row(base) == 1,
            "new row was not activated");
    for (int i = 0; i < 3; ++i) timed.tick();
    require(timed.prepare(20, p::Operation::Read, base + 64, 4), "row conflict never finished");
    (void)memory.read(base + 64, 4); timed.complete(20, base + 64);
    timed.reset();
    require(!timed.prepare(21, p::Operation::Read, base + 64, 4), "new hit was ready");
    timed.tick();
    require(timed.scheduler().last_tick().command == Scheduler::Command::Access, "reset forgot DIMM row");
    timed.reset();

    // Four separate Split Fabric jobs can overlap. A single burst, however,
    // exposes its beats sequentially via prepare/release.
    p::Fabric fabric;
    p::Endpoint endpoint{base, module.geometry().capacity_bytes(), 1,
        [&](auto address, auto bytes) { return memory.read(address, bytes); },
        [&](auto address, const auto& bytes) { memory.write(address, bytes); }, "interleaved DIMM"};
    endpoint.prepare = [&](auto tag, auto op, auto address, auto bytes) { return timed.prepare(tag, op, address, bytes); };
    endpoint.release = [&](auto tag, auto address) { timed.complete(tag, address); };
    fabric.map(std::move(endpoint));
    std::vector<p::Tag> tags;
    for (unsigned i = 0; i < 4; ++i)
        tags.push_back(fabric.submit({i, p::Operation::Read, base + 4 * i, 4, 1, true, {}}));
    bool overlapping = false;
    for (unsigned tick = 0; tick < 100 && !fabric.idle(); ++tick) {
        timed.tick(); fabric.tick();
        overlapping |= timed.scheduler().statistics().accesses > timed.scheduler().statistics().ready + 1;
    }
    require(fabric.idle() && overlapping && !timed.busy(), "split requests did not overlap");
    for (unsigned i = 0; i < 4; ++i) {
        require(fabric.ready(tags[i]), "missing response");
        auto result = fabric.take(tags[i]);
        require(result.status == p::Status::Success && devices::common::load32(result.data) == 0x12340000 + i,
                "incorrect fabric read");
    }
    const auto burst = fabric.submit({0, p::Operation::Write, base, 4, 4, true,
        {1,0,0,0, 2,0,0,0, 3,0,0,0, 4,0,0,0}});
    for (unsigned tick = 0; tick < 200 && !fabric.ready(burst); ++tick) { timed.tick(); fabric.tick(); }
    require(fabric.ready(burst) && fabric.take(burst).status == p::Status::Success, "burst write failed");
    for (unsigned i = 0; i < 4; ++i)
        require(devices::common::load32(memory.inspect(base + 4 * i, 4)) == i + 1, "burst data mismatch");

    bool rejected = false;
    try { timed.prepare(100, p::Operation::Read, base + 3, 2); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "cross-word beat accepted");
    SoCConfig cfg;
    cfg.enable_timed_dram = true;
    cfg.dram_mode = DramScheduleMode::InterleavedPipeline;
    rejected = false;
    try { (void)cfg.selected_dram_mode(); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "simultaneous scheduling modes accepted");
    cfg.enable_timed_dram = false;
    cfg.memory.rank.dram_chip.banks = 4;
    SoC system({0x00000063}, cfg);
    const auto system_tag = system.bus().submit(0, p::Operation::Write, 0x100, 4,
                                                devices::common::word_bytes(0xfeedface));
    for (unsigned i = 0; i < 200 && !system.bus().ready(system_tag); ++i) system.tick();
    require(system.bus().ready(system_tag) && system.bus().take(system_tag).status == p::Status::Success &&
            devices::common::load32(system.memory().inspect(0x100, 4)) == 0xfeedface &&
            system.interleaved_dram()->scheduler().statistics().accesses > 0 && system.dram_timing().cycles() == 0,
            "SoC did not route writes through the interleaved scheduler");
    std::cout << "PASS interleaved DRAM: real DIMM commands, reset, inspect and Fabric reads/writes\n";
}
