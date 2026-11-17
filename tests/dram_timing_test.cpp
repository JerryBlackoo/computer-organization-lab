#include "../soc/controller/memory/dram_timing.hpp"
#include "../devices/common/mmio_device.hpp"

#include <cassert>
#include <iostream>
#include <vector>

using namespace colab;
namespace p = interconnect::pcie;
using Dram = soc::controller::DramTiming;

int main() {
    dimm::DIMM module({1, {4, {2, {4, 4, 8}}}});
    soc::controller::Memory memory(module);
    Dram dram(memory, {2, 3, 4});
    p::Fabric fabric;
    p::Endpoint endpoint{0, module.geometry().capacity_bytes(), 1,
        [&](auto address, auto bytes) { return memory.read(address, bytes); },
        [&](auto address, const auto& bytes) { memory.write(address, bytes); }, "timed DRAM"};
    endpoint.prepare = [&](auto tag, auto op, auto address, auto bytes) { return dram.prepare(tag, op, address, bytes); };
    endpoint.release = [&](auto tag, auto address) { dram.complete(tag, address); };
    fabric.map(std::move(endpoint));
    std::vector<Dram::Trace> trace;
    auto tick = [&] { dram.tick(); trace.push_back(dram.last_tick()); fabric.tick(); };
    auto transfer = [&](std::uint32_t addr, bool write, std::uint32_t value = 0) {
        const auto tag = fabric.submit({0, write ? p::Operation::Write : p::Operation::Read, addr, 4, 1, true,
                                        write ? devices::common::word_bytes(value) : p::Bytes{}});
        for (unsigned n = 0; n < 100 && !fabric.ready(tag); ++n) tick();
        assert(fabric.ready(tag)); return fabric.take(tag);
    };
    // First access: ACT -> tRCD -> WRITE -> tCAS -> completion.
    assert(transfer(0, true, 0x12345678).status == p::Status::Success);
    std::uint64_t act = 0, write = 0, ready = 0;
    for (const auto& event : trace) {
        if (event.command == Dram::Command::Activate) act = event.cycle;
        if (event.command == Dram::Command::Write) write = event.cycle;
        if (event.command == Dram::Command::DataReady) ready = event.cycle;
    }
    assert(act && write - act == 2 && ready - write == 4);
    trace.clear();
    const auto hit = transfer(8, false); // Same bank and row, different column.
    assert(hit.status == p::Status::Success && devices::common::load32(hit.data) == 0);
    for (const auto& event : trace) assert(event.command != Dram::Command::Activate && event.command != Dram::Command::Precharge);
    trace.clear();
    // Bank 0, row 1: 4 columns * 2 banks * 4 bytes.
    assert(transfer(32, true, 7).status == p::Status::Success);
    std::uint64_t pre = 0, next_act = 0, next_write = 0;
    for (const auto& event : trace) {
        if (event.command == Dram::Command::Precharge) pre = event.cycle;
        if (event.command == Dram::Command::Activate) next_act = event.cycle;
        if (event.command == Dram::Command::Write) next_write = event.cycle;
    }
    assert(pre && next_act - pre == 3 && next_write - next_act == 2);
    assert(devices::common::load32(transfer(0, false).data) == 0x12345678);
    assert(devices::common::load32(transfer(32, false).data) == 7);
    // A debugger can inspect another row while a timed transaction is in
    // flight without issuing commands or disturbing the selected row.
    const auto pending = fabric.submit({0, p::Operation::Read, 0, 4, 1, true, {}});
    for (unsigned n = 0; n < 100 && dram.phase() != Dram::Phase::WaitData; ++n) tick();
    assert(dram.phase() == Dram::Phase::WaitData);
    const auto row_before = memory.active_row(0);
    const auto reads_before = memory.statistics().read_requests;
    assert(devices::common::load32(memory.inspect(32, 4)) == 7);
    assert(memory.active_row(0) == row_before && memory.statistics().read_requests == reads_before);
    for (unsigned n = 0; n < 100 && !fabric.ready(pending); ++n) tick();
    assert(fabric.ready(pending) && devices::common::load32(fabric.take(pending).data) == 0x12345678);
    const auto first = fabric.submit({1, p::Operation::Read, 0, 4, 1, true, {}});
    const auto second = fabric.submit({2, p::Operation::Read, 32, 4, 1, true, {}});
    for (unsigned n = 0; n < 200 && (!fabric.ready(first) || !fabric.ready(second)); ++n) tick();
    assert(fabric.ready(first) && fabric.ready(second));
    assert(devices::common::load32(fabric.take(first).data) == 0x12345678 &&
           devices::common::load32(fabric.take(second).data) == 7 && !dram.busy());
    // A synchronous fabric has a fixed readiness budget. A timed-out beat
    // must also release the scheduler so a later transaction can proceed.
    p::Fabric sync({p::Timing::Synchronous, 2, 8});
    p::Endpoint slow{0, module.geometry().capacity_bytes(), 1,
        [&](auto address, auto bytes) { return memory.read(address, bytes); },
        [&](auto address, const auto& bytes) { memory.write(address, bytes); }, "timed DRAM"};
    slow.prepare = [&](auto tag, auto op, auto address, auto bytes) { return dram.prepare(tag, op, address, bytes); };
    slow.release = [&](auto tag, auto address) { dram.complete(tag, address); };
    slow.cancel = [&](auto tag, auto address) { dram.cancel(tag, address); };
    sync.map(std::move(slow));
    for (unsigned i = 0; i < 2; ++i) {
        auto tag = sync.submit({0, p::Operation::Read, 0, 4, 1, true, {}});
        for (unsigned n = 0; n < 20 && !sync.ready(tag); ++n) { dram.tick(); sync.tick(); }
        assert(sync.ready(tag) && sync.take(tag).status == p::Status::Timeout && !dram.busy());
    }
    p::Fabric ready_sync({p::Timing::Synchronous, 4, 8});
    unsigned polls = 0, releases = 0;
    p::Endpoint delayed{0, 16, 1, [](auto, auto n) { return p::Bytes(n, 7); }, {}, "delayed"};
    delayed.prepare = [&](auto, auto, auto, auto) { return ++polls >= 3; };
    delayed.release = [&](auto, auto) { ++releases; };
    ready_sync.map(std::move(delayed));
    const auto ready_tag = ready_sync.submit({0, p::Operation::Read, 0, 4, 1, true, {}});
    ready_sync.run(20);
    assert(ready_sync.take(ready_tag).status == p::Status::Success && polls == 3 && releases == 1);
    p::Fabric burst_sync({p::Timing::Synchronous, 4, 8});
    unsigned beat_polls = 0, beat_releases = 0;
    p::Endpoint burst_endpoint{0, 16, 1, [](auto, auto n) { return p::Bytes(n, 9); }, {}, "burst"};
    burst_endpoint.prepare = [&](auto, auto, auto, auto) { return ++beat_polls >= 3; };
    burst_endpoint.release = [&](auto, auto) { ++beat_releases; beat_polls = 0; };
    burst_sync.map(std::move(burst_endpoint));
    const auto burst_tag = burst_sync.submit({0, p::Operation::Read, 0, 4, 2, true, {}});
    burst_sync.run(40);
    assert(burst_sync.take(burst_tag).status == p::Status::Success && beat_releases == 2 &&
           burst_sync.statistics().address_phases == 1);
    std::cout << "PASS DRAM: PRE/ACT/CAS timing, row hit/conflict and acknowledged data beats\n";
}
