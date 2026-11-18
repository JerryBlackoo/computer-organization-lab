#pragma once

#include "memory.hpp"
#include "../../../interconnect/fabric.hpp"

namespace colab::soc::controller {

// Single-issue command scheduler for a DIMM data beat. The fabric owns data
// transfer; this object schedules real PRE/ACT commands and CAS readiness.
class DramTiming {
public:
    struct Config { unsigned tRCD = 2, tRP = 2, tCAS = 2; };
    enum class Phase { Idle, Precharge, Activate, Column, WaitData, Ready };
    enum class Command { None, Precharge, Activate, Read, Write, DataReady };
    struct Statistics { std::uint64_t precharges = 0, activations = 0, reads = 0, writes = 0, ready = 0; };
    struct Trace { std::uint64_t cycle = 0; Phase phase = Phase::Idle; Command command = Command::None;
                   std::uint32_t address = 0; };
    explicit DramTiming(Memory& memory, Config config) : memory_(memory), config_(config) {
        if (!config.tRCD || !config.tRP || !config.tCAS) throw std::invalid_argument("DRAM timings must be positive");
    }
    void reset() { request_.reset(); phase_ = Phase::Idle; due_ = 0; available_ = 0; trace_ = {}; stats_ = {}; cycle_ = 0; }
    bool busy() const { return request_.has_value(); }
    Phase phase() const { return phase_; }
    const Trace& last_tick() const { return trace_; }
    std::uint64_t cycles() const { return cycle_; }
    const Statistics& statistics() const { return stats_; }

    bool prepare(interconnect::fabric::Tag tag, interconnect::fabric::Operation operation,
                 std::uint32_t address, std::size_t bytes) {
        if (!bytes) throw std::invalid_argument("empty DRAM beat");
        if (request_) {
            if (request_->tag != tag || request_->address != address || request_->bytes != bytes || request_->op != operation)
                return false;
            return phase_ == Phase::Ready;
        }
        const auto first = memory_.decode(address);
        if (bytes - 1 > UINT32_MAX - address || memory_.decode(address + static_cast<std::uint32_t>(bytes - 1)).word_address != first.word_address)
            throw std::invalid_argument("DRAM beat crosses a rank word");
        request_ = Request{tag, operation, address, bytes};
        const auto open = memory_.active_row(address);
        phase_ = open ? (*open == first.dram.row ? Phase::Column : Phase::Precharge) : Phase::Activate;
        due_ = std::max(cycle_ + 1, available_);
        return false;
    }
    void complete(interconnect::fabric::Tag tag, std::uint32_t address) {
        if (!request_ || request_->tag != tag || request_->address != address || phase_ != Phase::Ready)
            throw std::logic_error("DRAM data beat completed without readiness");
        request_.reset(); phase_ = Phase::Idle;
    }
    void cancel(interconnect::fabric::Tag tag, std::uint32_t address) {
        if (request_ && request_->tag == tag && request_->address == address) {
            available_ = std::max(available_, due_);
            request_.reset(); phase_ = Phase::Idle;
        }
    }
    void tick() {
        trace_ = {++cycle_, phase_, Command::None, request_ ? request_->address : 0};
        if (!request_ || cycle_ < due_) return;
        const auto address = request_->address;
        switch (phase_) {
        case Phase::Precharge:
            memory_.precharge(address); trace_.command = Command::Precharge;
            ++stats_.precharges;
            phase_ = Phase::Activate; due_ = cycle_ + config_.tRP; break;
        case Phase::Activate:
            memory_.activate(address); trace_.command = Command::Activate;
            ++stats_.activations;
            phase_ = Phase::Column; due_ = cycle_ + config_.tRCD; break;
        case Phase::Column:
            trace_.command = request_->op == interconnect::fabric::Operation::Read ? Command::Read : Command::Write;
            if (request_->op == interconnect::fabric::Operation::Read) ++stats_.reads; else ++stats_.writes;
            phase_ = Phase::WaitData; due_ = cycle_ + config_.tCAS; break;
        case Phase::WaitData:
            trace_.command = Command::DataReady; ++stats_.ready; phase_ = Phase::Ready; break;
        case Phase::Idle: case Phase::Ready: break;
        }
    }
private:
    struct Request { interconnect::fabric::Tag tag; interconnect::fabric::Operation op; std::uint32_t address; std::size_t bytes; };
    Memory& memory_;
    Config config_;
    std::optional<Request> request_;
    Phase phase_ = Phase::Idle;
    std::uint64_t cycle_ = 0, due_ = 0, available_ = 0;
    Trace trace_{};
    Statistics stats_{};
};
} // namespace colab::soc::controller
