#pragma once

#include "../../../dimm/geometry.hpp"
#include "../../../interconnect/fabric.hpp"

#include <cstdint>
#include <functional>
#include <list>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace colab::soc::controller {

// Teaching scheduler for DIMM words. Optional hooks issue real row commands;
// data movement remains the caller's responsibility. One instance is one DIMM.
class InterleavedDramScheduler {
public:
    struct Config {
        unsigned t_bus = 1; // Platform ticks between command slots.
        unsigned ta = 4;    // Access start boundary to data ready, in ticks.
        bool require_ta_equals_n_times_bus = true;
        unsigned tRP = 0;   // Earliest ACT after PRE, in ticks.
    };
    enum class Phase { Queued, Precharging, InFlight, Ready };
    enum class Command { None, Precharge, Activate, Access };
    struct Trace {
        std::uint64_t cycle = 0;
        Command command = Command::None;
        interconnect::fabric::Tag tag = 0;
        dimm::DIMMAddress address{};
    };
    struct Statistics {
        std::uint64_t precharges = 0, activations = 0, accesses = 0, ready = 0;
    };
    // Optional hooks bind timing decisions to a functional memory model.
    struct Hooks {
        std::function<std::optional<std::size_t>(std::size_t)> active_row;
        std::function<void(std::size_t)> precharge;
        std::function<void(std::size_t)> activate;
    };

    explicit InterleavedDramScheduler(dimm::DIMMGeometry geometry, Config config)
        : InterleavedDramScheduler(geometry, config, Hooks{}) {}
    InterleavedDramScheduler(dimm::DIMMGeometry geometry, Config config, Hooks hooks)
        : geometry_(geometry), config_(config), hooks_(std::move(hooks)) {
        geometry_.validate();
        if ((hooks_.active_row || hooks_.precharge || hooks_.activate) &&
            (!hooks_.active_row || !hooks_.precharge || !hooks_.activate))
            throw std::invalid_argument("all DRAM hooks are required together");
        if (!config_.t_bus || !config_.ta)
            throw std::invalid_argument("DRAM bus period and access time must be positive");
        if (config_.require_ta_equals_n_times_bus &&
            (config_.ta % config_.t_bus != 0 ||
             config_.ta / config_.t_bus != geometry_.rank.dram_chip.banks))
            throw std::invalid_argument("ta must equal banks times t_bus");
        banks_.resize(dimm::detail::multiply(geometry_.ranks, geometry_.rank.dram_chip.banks));
    }

    // word_address is DIMM-local, in full Rank words, not physical bytes.
    // Repeated prepare for the same tag is a poll; a tag cannot change identity.
    bool prepare(interconnect::fabric::Tag tag, interconnect::fabric::Operation op,
                 std::size_t word_address) {
        if (op != interconnect::fabric::Operation::Read && op != interconnect::fabric::Operation::Write)
            throw std::invalid_argument("unknown DRAM operation");
        for (const auto& request : requests_) {
            if (request.tag != tag) continue;
            if (request.op != op || request.word_address != word_address)
                throw std::invalid_argument("DRAM tag reused for a different beat");
            return request.phase == Phase::Ready;
        }
        const auto address = decode(word_address);
        requests_.push_back({tag, op, word_address, address, Phase::Queued, 0});
        return false;
    }

    // Completion acknowledges consumption; until then the bank stays reserved,
    // so a later row conflict cannot invalidate an unconsumed ready beat.
    void complete(interconnect::fabric::Tag tag) {
        for (auto it = requests_.begin(); it != requests_.end(); ++it) {
            if (it->tag != tag) continue;
            if (it->phase != Phase::Ready) throw std::logic_error("DRAM beat is not ready");
            banks_[bank_index(it->address)].reserved = false;
            requests_.erase(it);
            return;
        }
        throw std::logic_error("unknown DRAM tag");
    }
    void cancel(interconnect::fabric::Tag tag) {
        for (auto it = requests_.begin(); it != requests_.end(); ++it) {
            if (it->tag != tag) continue;
            if (it->phase == Phase::InFlight || it->phase == Phase::Ready) {
                auto& bank = banks_[bank_index(it->address)];
                bank.available_at = std::max(bank.available_at, it->ready_at + 1);
                bank.reserved = false;
            }
            requests_.erase(it);
            return;
        }
    }

    void tick() {
        trace_ = {};
        trace_.cycle = ++cycle_;
        for (auto& request : requests_) {
            if (request.phase == Phase::InFlight && request.ready_at <= cycle_) {
                request.phase = Phase::Ready;
                ++stats_.ready;
            }
        }
        if (cycle_ < next_command_) return;
        for (auto it = requests_.begin(); it != requests_.end(); ++it) {
            if (it->phase != Phase::Queued && it->phase != Phase::Precharging) continue;
            auto& bank = banks_[bank_index(it->address)];
            if (bank.reserved || cycle_ < bank.available_at || !is_bank_head(it)) continue;
            trace_.tag = it->tag;
            trace_.address = it->address;
            const auto open_row = hooks_.active_row ? hooks_.active_row(it->word_address) : bank.open_row;
            if (open_row && *open_row != it->address.row) {
                if (hooks_.precharge) hooks_.precharge(it->word_address);
                trace_.command = Command::Precharge;
                ++stats_.precharges;
                bank.open_row.reset();
                bank.available_at = cycle_ + config_.tRP;
                it->phase = Phase::Precharging;
            } else {
                if (!open_row) {
                    if (hooks_.activate) hooks_.activate(it->word_address);
                    trace_.command = Command::Activate;
                    ++stats_.activations;
                    bank.open_row = it->address.row;
                } else {
                    trace_.command = Command::Access;
                }
                ++stats_.accesses;
                bank.reserved = true;
                it->phase = Phase::InFlight;
                // A command issued during tick 1 starts at boundary 0.
                it->ready_at = cycle_ + config_.ta - 1;
                if (it->ready_at == cycle_) {
                    it->phase = Phase::Ready;
                    ++stats_.ready;
                }
            }
            next_command_ = cycle_ + config_.t_bus;
            return;
        }
    }

    void reset() {
        requests_.clear();
        for (auto& bank : banks_) bank = {};
        cycle_ = 0; next_command_ = 0; trace_ = {}; stats_ = {};
    }
    bool busy() const { return !requests_.empty(); }
    std::uint64_t cycles() const { return cycle_; }
    const Trace& last_tick() const { return trace_; }
    const Statistics& statistics() const { return stats_; }

private:
    struct Request {
        interconnect::fabric::Tag tag;
        interconnect::fabric::Operation op;
        std::size_t word_address;
        dimm::DIMMAddress address;
        Phase phase;
        std::uint64_t ready_at;
    };
    struct BankState {
        std::optional<std::size_t> open_row;
        std::uint64_t available_at = 0;
        bool reserved = false;
    };
    using Iterator = std::list<Request>::const_iterator;
    std::size_t bank_index(dimm::DIMMAddress address) const {
        return address.rank * geometry_.rank.dram_chip.banks + address.bank;
    }
    bool is_bank_head(Iterator candidate) const {
        for (auto it = requests_.cbegin(); it != candidate; ++it)
            if (bank_index(it->address) == bank_index(candidate->address)) return false;
        return true;
    }
    dimm::DIMMAddress decode(std::size_t word) const {
        if (word >= geometry_.word_count()) throw std::out_of_range("word outside DIMM");
        const auto rank = word / geometry_.rank.word_count();
        const auto local = word % geometry_.rank.word_count();
        const auto banks = geometry_.rank.dram_chip.banks;
        const auto in_bank = local / banks;
        return {rank, local % banks, in_bank / geometry_.rank.dram_chip.bank.columns,
                in_bank % geometry_.rank.dram_chip.bank.columns};
    }
    dimm::DIMMGeometry geometry_;
    Config config_;
    Hooks hooks_;
    std::vector<BankState> banks_;
    std::list<Request> requests_;
    std::uint64_t cycle_ = 0, next_command_ = 0;
    Trace trace_{};
    Statistics stats_{};
};
} // namespace colab::soc::controller
