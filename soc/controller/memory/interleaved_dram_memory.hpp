#pragma once

#include "interleaved_dram.hpp"
#include "memory.hpp"

#include <limits>
#include <map>

namespace colab::soc::controller {

// One mapped DIMM. The Fabric still performs data reads/writes after prepare
// returns true, then calls complete. Host functional accesses must not mutate
// the DIMM while requests are in flight; inspect is safe.
class InterleavedDramMemory {
public:
    using Scheduler = InterleavedDramScheduler;
    InterleavedDramMemory(Memory& memory, Memory::Address base, Scheduler::Config config)
        : memory_(memory), base_(base), width_(memory.geometry(base).word_bits() / 8),
          scheduler_(memory.geometry(base), config,
              {[this](std::size_t word) { return memory_.active_row(physical(word)); },
               [this](std::size_t word) { memory_.precharge(physical(word)); },
               [this](std::size_t word) { memory_.activate(physical(word)); }}) {
        if (memory.decode(base).local_byte_address != 0)
            throw std::invalid_argument("interleaved DRAM base must be a DIMM window base");
    }
    InterleavedDramMemory(const InterleavedDramMemory&) = delete;
    InterleavedDramMemory& operator=(const InterleavedDramMemory&) = delete;
    InterleavedDramMemory(InterleavedDramMemory&&) = delete;
    InterleavedDramMemory& operator=(InterleavedDramMemory&&) = delete;

    bool prepare(interconnect::fabric::Tag tag, interconnect::fabric::Operation op,
                 Memory::Address address, std::size_t bytes) {
        if (!bytes || bytes - 1 > std::numeric_limits<Memory::Address>::max() - address)
            throw std::invalid_argument("invalid DRAM beat size");
        const auto first = memory_.decode(address);
        const auto last = memory_.decode(address + static_cast<Memory::Address>(bytes - 1));
        if (address < base_ || address - first.local_byte_address != base_ ||
            first.word_address != last.word_address ||
            last.local_byte_address != first.local_byte_address + bytes - 1)
            throw std::invalid_argument("DRAM beat crosses a rank word or DIMM window");
        const auto existing = requests_.find(tag);
        if (existing != requests_.end() &&
            (existing->second.address != address || existing->second.bytes != bytes || existing->second.op != op))
            throw std::invalid_argument("DRAM tag reused for a different beat");
        const bool ready = scheduler_.prepare(tag, op, first.word_address);
        if (existing == requests_.end()) requests_.emplace(tag, Request{op, address, bytes});
        return ready;
    }
    void complete(interconnect::fabric::Tag tag, Memory::Address address) {
        const auto it = requests_.find(tag);
        if (it == requests_.end() || it->second.address != address)
            throw std::logic_error("unknown DRAM beat completion");
        scheduler_.complete(tag);
        requests_.erase(it);
    }
    void cancel(interconnect::fabric::Tag tag, Memory::Address address) {
        const auto it = requests_.find(tag);
        if (it == requests_.end() || it->second.address != address) return;
        scheduler_.cancel(tag);
        requests_.erase(it);
    }
    void tick() { scheduler_.tick(); }
    void reset() { scheduler_.reset(); requests_.clear(); }
    bool busy() const { return scheduler_.busy(); }
    const Scheduler& scheduler() const { return scheduler_; }
private:
    struct Request { interconnect::fabric::Operation op; Memory::Address address; std::size_t bytes; };
    Memory::Address physical(std::size_t word) const {
        return static_cast<Memory::Address>(std::uint64_t{base_} + std::uint64_t{word} * width_);
    }
    Memory& memory_;
    Memory::Address base_;
    std::size_t width_;
    Scheduler scheduler_;
    std::map<interconnect::fabric::Tag, Request> requests_;
};
} // namespace colab::soc::controller
