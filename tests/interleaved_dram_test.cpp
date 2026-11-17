#include "../soc/controller/memory/interleaved_dram.hpp"
#include "../dimm/dimm.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace colab;
using Scheduler = soc::controller::InterleavedDramScheduler;
namespace p = interconnect::pcie;

int main() {
    const dimm::DIMMGeometry geometry{2, {4, {4, {2, 4, 8}}}};
    dimm::DIMM module(geometry);
    Scheduler dram(geometry, {1, 4, true, 2});

    // Four consecutive rank words: banks 0,1,2,3. A tick ends at the
    // numbered boundary, so ACT during tick 1 starts at boundary 0.
    for (std::size_t word = 0; word < 4; ++word) {
        assert(module.decode(word).bank == word);
        assert(!dram.prepare(word, p::Operation::Read, word));
    }
    std::vector<std::uint64_t> issued, ready;
    for (unsigned tick = 1; tick <= 7; ++tick) {
        dram.tick();
        const auto event = dram.last_tick();
        if (event.command == Scheduler::Command::Activate) {
            issued.push_back(event.cycle);
            assert(event.address.bank == issued.size() - 1);
        }
        for (std::size_t word = 0; word < 4; ++word) {
            if (word + 4 > tick) {
                assert(!dram.prepare(word, p::Operation::Read, word));
                continue;
            }
            if (word + 4 < tick) continue; // Never poll a completed tag again.
            assert(dram.prepare(word, p::Operation::Read, word));
            ready.push_back(dram.cycles());
            dram.complete(word);
        }
    }
    assert((issued == std::vector<std::uint64_t>{1, 2, 3, 4}));
    assert((ready == std::vector<std::uint64_t>{4, 5, 6, 7}));
    assert(ready.back() == 4 + (4 - 1));
    assert(!dram.busy() && dram.statistics().activations == 4);

    // A ready-but-unconsumed beat protects its row; another bank can progress.
    dram.reset();
    assert(!dram.prepare(10, p::Operation::Write, 0));
    assert(!dram.prepare(11, p::Operation::Read, 16)); // bank 0, row 1
    assert(!dram.prepare(12, p::Operation::Read, 1));  // bank 1 bypasses
    for (int i = 0; i < 8; ++i) {
        dram.tick();
        assert(dram.last_tick().tag != 11 || dram.last_tick().command == Scheduler::Command::None);
    }
    assert(dram.prepare(10, p::Operation::Write, 0));
    assert(dram.prepare(12, p::Operation::Read, 1));
    dram.complete(10);
    dram.complete(12);
    dram.tick();
    assert(dram.last_tick().tag == 11 && dram.last_tick().command == Scheduler::Command::Precharge);
    const auto pre = dram.cycles();
    dram.tick();
    assert(dram.last_tick().command == Scheduler::Command::None);
    dram.tick();
    assert(dram.last_tick().tag == 11 && dram.last_tick().command == Scheduler::Command::Activate);
    assert(dram.cycles() - pre == 2);
    for (int i = 0; i < 3; ++i) dram.tick();
    assert(dram.prepare(11, p::Operation::Read, 16));
    dram.complete(11);
    // Hit uses a command slot without another ACT.
    const auto acts = dram.statistics().activations;
    assert(!dram.prepare(13, p::Operation::Read, 20));
    dram.tick();
    assert(dram.last_tick().command == Scheduler::Command::Access);
    assert(dram.statistics().activations == acts);
    for (int i = 0; i < 3; ++i) dram.tick();
    assert(dram.prepare(13, p::Operation::Read, 20));
    dram.complete(13);

    // Two-tick bus: ACT at tick 1,3,5,7; ready at 8,10,12,14.
    Scheduler slow(geometry, {2, 8, true, 0});
    for (std::size_t word = 0; word < 4; ++word) slow.prepare(word, p::Operation::Read, word);
    unsigned commands = 0;
    for (unsigned t = 1; t <= 14; ++t) {
        slow.tick();
        if (slow.last_tick().command != Scheduler::Command::None) {
            assert(t == 1 + 2 * commands++);
        }
        if (t >= 8 && t % 2 == 0) {
            const auto tag = (t - 8) / 2;
            assert(slow.prepare(tag, p::Operation::Read, tag));
            slow.complete(tag);
        }
    }
    assert(commands == 4 && slow.cycles() == 8 + 3 * 2);

    // Independent ranks and input validation.
    Scheduler ranks(geometry, {1, 4, true, 0});
    const auto rank1word = geometry.rank.word_count();
    ranks.prepare(1, p::Operation::Read, 0);
    ranks.prepare(2, p::Operation::Read, rank1word);
    ranks.tick(); ranks.tick();
    assert(ranks.last_tick().address.rank == 1 && ranks.last_tick().address.bank == 0);
    bool rejected = false;
    try { ranks.prepare(1, p::Operation::Write, 0); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    rejected = false;
    try { ranks.complete(1); } catch (const std::logic_error&) { rejected = true; }
    assert(rejected);
    rejected = false;
    try { Scheduler invalid(geometry, {1, 3, true, 0}); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    rejected = false;
    try { ranks.prepare(3, p::Operation::Read, geometry.word_count()); } catch (const std::out_of_range&) { rejected = true; }
    assert(rejected);
    ranks.reset();
    assert(!ranks.busy() && ranks.cycles() == 0 && ranks.statistics().activations == 0);
    // Cancelling an in-flight beat releases its reservation but cannot make
    // the bank available before the originally scheduled data boundary.
    Scheduler cancelled(geometry, {1, 4, true, 0});
    cancelled.prepare(30, p::Operation::Read, 0);
    cancelled.tick();
    cancelled.cancel(30);
    cancelled.prepare(31, p::Operation::Read, 0);
    for (unsigned t = 2; t <= 4; ++t) {
        cancelled.tick();
        assert(cancelled.last_tick().command == Scheduler::Command::None);
    }
    cancelled.tick();
    assert(cancelled.last_tick().tag == 31 && cancelled.last_tick().command == Scheduler::Command::Access);
    std::cout << "PASS interleaved DRAM: pipeline, command slots, bank conflicts and timing\n";
}
