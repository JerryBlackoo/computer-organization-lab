#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace colab::interconnect::pcie {
using Address = std::uint32_t;
using Bytes = std::vector<std::uint8_t>;
using Tag = std::uint64_t;
using Master = std::uint32_t;
enum class Timing { Synchronous, Asynchronous, SemiSynchronous, Split };
enum class Operation { Read, Write };
enum class Status { Success, Unmapped, BackendError, Timeout };
enum class Phase { Idle, Address, Wait, Ready, Response, Data, Release };
enum class Sender { None, Requester, Endpoint };
struct Config {
    Timing timing = Timing::Split;
    unsigned synchronous_wait = 1;
    std::size_t max_outstanding = 64;
};
struct Request {
    Master master = 0;
    Operation operation = Operation::Read;
    Address address = 0;
    std::size_t beat_bytes = 4, beats = 1;
    bool burst = true;
    Bytes data;
};
struct Result {
    Tag tag;
    Master master;
    Status status;
    Bytes data;
    std::size_t transferred_bytes;
    std::string error;
    // Only a split READ has a modeled returned completion. Writes are posted.
    bool read_completion;
};
struct Trace {
    std::uint64_t tick = 0;
    Phase phase = Phase::Idle;
    std::optional<Tag> tag;
    std::optional<Master> master;
    std::optional<Address> address;
    bool req = false, ack = false, wait_n = true, link_busy = false;
    bool completed = false;
    Bytes data;
    Sender sender = Sender::None;
    std::optional<std::size_t> endpoint;
};
struct Statistics {
    std::uint64_t ticks = 0, address_phases = 0, response_phases = 0;
    std::uint64_t data_beats = 0, bytes = 0, wait_ticks = 0, idle_ticks = 0;
    std::uint64_t completed = 0, errors = 0, read_completions = 0, posted_writes = 0;
};
struct Endpoint {
    Address base;
    std::uint64_t size;
    unsigned preparation_ticks;
    std::function<Bytes(Address, std::size_t)> read;
    std::function<void(Address, const Bytes&)> write;
    std::string name;
};

// Shared TRANSACTION scheduler for textbook timings. NOT a PCIe wire/PHY simulator.
class Fabric {
public:
    explicit Fabric(Config config = {}) : config_(config) {
        if (!config_.synchronous_wait || !config_.max_outstanding)
            throw std::invalid_argument("positive timing and queue limits required");
        switch (config_.timing) {
        case Timing::Synchronous: case Timing::Asynchronous:
        case Timing::SemiSynchronous: case Timing::Split: break;
        default: throw std::invalid_argument("unknown timing mode");
        }
    }
    void map(Endpoint endpoint) {
        if (!idle()) throw std::logic_error("cannot change address map during transactions");
        constexpr auto space = std::uint64_t{1} << 32;
        if (!endpoint.size || endpoint.size > space - endpoint.base)
            throw std::invalid_argument("invalid endpoint window");
        if (!endpoint.read && !endpoint.write) throw std::invalid_argument("endpoint has no port");
        for (const auto& e : endpoints_)
            if (std::uint64_t(endpoint.base) < std::uint64_t(e.base) + e.size &&
                std::uint64_t(e.base) < std::uint64_t(endpoint.base) + endpoint.size)
                throw std::invalid_argument("overlapping endpoint windows");
        endpoints_.push_back(std::move(endpoint));
    }
    Tag submit(Request request) {
        validate(request);
        if (outstanding_ == config_.max_outstanding) throw std::length_error("transaction queue full");
        if (next_tag_ == std::numeric_limits<Tag>::max()) throw std::overflow_error("tag space exhausted");
        const auto tag = next_tag_++;
        Job job;
        job.tag = tag; job.request = std::move(request);
        requests_.push_back(std::move(job));
        ++outstanding_;
        return tag;
    }
    bool idle() const { return outstanding_ == 0; }
    std::size_t outstanding() const { return outstanding_; }
    const Statistics& statistics() const { return stats_; }
    const Trace& last_tick() const { return trace_; }
    bool ready(Tag tag) const { return results_.count(tag) != 0; }
    Result take(Tag tag) {
        auto found = results_.find(tag);
        if (found == results_.end()) throw std::out_of_range("result is not ready");
        auto result = std::move(found->second);
        results_.erase(found);
        return result;
    }
    void run(std::uint64_t limit = 100000) {
        while (!idle() && limit) { tick(); --limit; }
        if (!idle()) throw std::runtime_error("fabric run limit exceeded (requests retained)");
    }
    void tick() {
        trace_ = {};
        trace_.tick = ++stats_.ticks;
        if (config_.timing == Timing::Split) tick_split();
        else tick_exclusive();
        if (trace_.phase == Phase::Idle) ++stats_.idle_ticks;
    }

private:
    struct Job {
        Tag tag = 0;
        Request request;
        std::size_t endpoint = 0, beat = 0;
        std::uint64_t ready_at = 0;
        Phase phase = Phase::Address;
        bool mapped = false, req = false, ack = false;
        Status status = Status::Success;
        std::string error;
        Bytes received;
    };
    static void validate(const Request& r) {
        if (r.operation != Operation::Read && r.operation != Operation::Write)
            throw std::invalid_argument("unknown operation");
        if (!r.beat_bytes || r.beat_bytes > 8 || (r.beat_bytes & (r.beat_bytes - 1)) ||
            !r.beats || r.beats > 1024 || r.address % r.beat_bytes)
            throw std::invalid_argument("unaligned or invalid transfer geometry");
        const auto bytes = r.beat_bytes * r.beats;
        if (bytes > (std::uint64_t{1} << 32) - r.address)
            throw std::out_of_range("transfer exceeds 32-bit address space");
        if ((r.operation == Operation::Write && r.data.size() != bytes) ||
            (r.operation == Operation::Read && !r.data.empty()))
            throw std::invalid_argument("payload length does not match operation");
    }
    Job arbitrate() {
        // Round-robin MASTER grants; FIFO within each master. No DMA priority override.
        auto chosen = requests_.end();
        for (auto it = requests_.begin(); it != requests_.end(); ++it) {
            if (last_master_ && it->request.master <= *last_master_) continue;
            if (chosen == requests_.end() || it->request.master < chosen->request.master) chosen = it;
        }
        if (chosen == requests_.end())
            chosen = std::min_element(requests_.begin(), requests_.end(),
                [](const Job& a, const Job& b) { return a.request.master < b.request.master; });
        Job job = std::move(*chosen);
        requests_.erase(chosen);
        last_master_ = job.request.master;
        return job;
    }
    Address address(const Job& j) const {
        return static_cast<Address>(std::uint64_t(j.request.address) + j.beat * j.request.beat_bytes);
    }
    void trace_job(const Job& j, Phase phase) {
        trace_.tag = j.tag; trace_.master = j.request.master;
        trace_.address = static_cast<Address>(std::uint64_t(j.request.address) +
            std::min(j.beat, j.request.beats - 1) * j.request.beat_bytes);
        trace_.phase = phase; trace_.link_busy = true;
        trace_.req = j.req; trace_.ack = j.ack;
        if (j.mapped) trace_.endpoint = j.endpoint;
        trace_.sender = (phase == Phase::Address ||
            (phase == Phase::Data && j.request.operation == Operation::Write))
            ? Sender::Requester : Sender::Endpoint;
    }
    void begin(Job& j) {
        trace_job(j, Phase::Address);
        ++stats_.address_phases;
        if (!j.mapped) {
            const auto count = j.request.beat_bytes * j.request.beats;
            for (std::size_t i = 0; i < endpoints_.size(); ++i) {
                const auto& e = endpoints_[i];
                if (j.request.address >= e.base &&
                    std::uint64_t(j.request.address) - e.base < e.size &&
                    count <= e.size - (std::uint64_t(j.request.address) - e.base)) {
                    j.endpoint = i; j.mapped = true; break;
                }
            }
            if (!j.mapped) {
                j.status = Status::Unmapped; j.error = "unmapped or crossing endpoint window";
                return;
            }
        }
        const auto latency = std::max(1u, endpoints_[j.endpoint].preparation_ticks);
        j.ready_at = stats_.ticks + latency;
        j.phase = Phase::Wait;
        if (config_.timing == Timing::Asynchronous) j.req = trace_.req = true;
    }
    bool transfer(Job& j) {
        trace_job(j, Phase::Data);
        const auto& e = endpoints_[j.endpoint];
        const auto n = j.request.beat_bytes;
        try {
            if (j.request.operation == Operation::Read) {
                if (!e.read) throw std::runtime_error("endpoint is not readable");
                auto bytes = e.read(address(j), n);
                if (bytes.size() != n) throw std::runtime_error("short endpoint read");
                j.received.insert(j.received.end(), bytes.begin(), bytes.end());
                trace_.data = std::move(bytes);
            } else {
                if (!e.write) throw std::runtime_error("endpoint is not writable");
                const auto start = j.request.data.begin() + j.beat * n;
                Bytes bytes(start, start + n);
                e.write(address(j), bytes);
                trace_.data = std::move(bytes);
            }
            ++stats_.data_beats; stats_.bytes += n; ++j.beat;
        } catch (const std::exception& error) {
            j.status = Status::BackendError; j.error = error.what();
        } catch (...) {
            j.status = Status::BackendError; j.error = "non-standard backend exception";
        }
        return j.status != Status::Success || j.beat == j.request.beats;
    }
    void finish(Job j) {
        const bool completion = config_.timing == Timing::Split && j.mapped &&
                                j.request.operation == Operation::Read;
        if (completion) ++stats_.read_completions;
        if (config_.timing == Timing::Split && j.request.operation == Operation::Write &&
            j.status == Status::Success) ++stats_.posted_writes;
        if (j.status != Status::Success) ++stats_.errors;
        results_.emplace(j.tag, Result{j.tag, j.request.master, j.status, std::move(j.received),
            j.beat * j.request.beat_bytes, std::move(j.error), completion});
        --outstanding_; ++stats_.completed; trace_.completed = true;
    }
    void tick_exclusive() {
        if (!active_) {
            if (requests_.empty()) return;
            active_ = arbitrate();
        }
        auto& j = *active_;
        if (j.phase == Phase::Address) {
            begin(j);
            if (j.status != Status::Success) { finish(std::move(j)); active_.reset(); }
            return;
        }
        if (j.phase == Phase::Wait) {
            trace_job(j, Phase::Wait);
            const bool sync = config_.timing == Timing::Synchronous;
            const auto elapsed = stats_.ticks - (j.ready_at - std::max(1u, endpoints_[j.endpoint].preparation_ticks));
            if ((sync && elapsed < config_.synchronous_wait) || (!sync && stats_.ticks < j.ready_at)) {
                ++stats_.wait_ticks;
                if (config_.timing == Timing::SemiSynchronous) trace_.wait_n = false;
                return;
            }
            if (sync && stats_.ticks < j.ready_at) {
                j.status = Status::Timeout; j.error = "device exceeded fixed synchronous preparation budget";
                finish(std::move(j)); active_.reset(); return;
            }
            trace_.phase = Phase::Response; ++stats_.response_phases;
            if (config_.timing == Timing::Asynchronous) j.ack = trace_.ack = true;
            j.phase = Phase::Data;
            return;
        }
        if (j.phase == Phase::Release) {
            j.ack = false;
            trace_job(j, Phase::Release);
            if (j.status != Status::Success || j.beat == j.request.beats) {
                finish(std::move(j)); active_.reset();
            } else j.phase = Phase::Address;
            return;
        }
        const bool done = transfer(j);
        if (!done && j.request.burst) return;
        if (config_.timing == Timing::Asynchronous) {
            j.req = trace_.req = false; // ACK stays high for this event, drops only next event.
            j.phase = Phase::Release;
        } else if (done) { finish(std::move(j)); active_.reset(); }
        else j.phase = Phase::Address;
    }
    void tick_split() {
        const auto ready = std::find_if(pending_.begin(), pending_.end(),
            [&](const Job& j) { return j.ready_at <= stats_.ticks; });
        if (ready != pending_.end() && (prefer_response_ || requests_.empty())) {
            Job j = std::move(*ready); pending_.erase(ready);
            prefer_response_ = false;
            if (j.phase == Phase::Address) {
                begin(j); pending_.push_back(std::move(j));
            } else if (j.phase == Phase::Wait) {
                if (j.request.operation == Operation::Read) {
                    trace_job(j, Phase::Response); ++stats_.response_phases;
                } else {
                    // A posted write has local queue readiness, NOT a returned completion.
                    trace_job(j, Phase::Ready);
                    trace_.link_busy = false; trace_.sender = Sender::None;
                }
                j.phase = Phase::Data; pending_.push_back(std::move(j));
            } else {
                if (transfer(j)) finish(std::move(j));
                else {
                    if (!j.request.burst) j.phase = Phase::Address;
                    pending_.push_back(std::move(j));
                }
            }
        } else if (!requests_.empty()) {
            auto j = arbitrate(); begin(j); prefer_response_ = true;
            if (j.status != Status::Success) finish(std::move(j));
            else pending_.push_back(std::move(j));
        }
        // While a slow endpoint prepares, no owner holds the modeled scheduler slot.
    }

    Config config_;
    std::vector<Endpoint> endpoints_;
    std::deque<Job> requests_, pending_;
    std::optional<Job> active_;
    std::optional<Master> last_master_;
    std::map<Tag, Result> results_;
    Tag next_tag_ = 1;
    std::size_t outstanding_ = 0;
    bool prefer_response_ = true;
    Statistics stats_;
    Trace trace_;
};
} // namespace colab::interconnect::pcie

namespace colab::interconnect {
// Retain the original public class name while exposing the explicit teaching engine.
using PCIe = pcie::Fabric;
}
