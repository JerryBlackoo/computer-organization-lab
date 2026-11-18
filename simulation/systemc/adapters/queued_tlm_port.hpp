#pragma once

#include "../../../devices/common/dma_bus.hpp"
#include <systemc>
#include <tlm>
#include <tlm_utils/simple_initiator_socket.h>
#include <deque>
#include <map>
#include <memory>

namespace colab::simulation::systemc {

struct BusTransaction : tlm::tlm_extension<BusTransaction> {
    devices::common::Master master = 0;
    devices::common::BusResult result{0, 0, interconnect::fabric::Status::BackendError, {}, 0, "no response", false};
    tlm::tlm_extension_base* clone() const override { return new BusTransaction(*this); }
    void copy_from(const tlm::tlm_extension_base& other) override {
        *this = static_cast<const BusTransaction&>(other);
    }
};

// Nonblocking model-facing queue, blocking TLM on a dedicated SystemC thread.
// submit() never waits or calls the remote hardware's business methods.
class QueuedTlmPort : public sc_core::sc_module, public devices::common::DmaBus {
public:
    tlm_utils::simple_initiator_socket<QueuedTlmPort> socket{"socket"};
#if SC_VERSION_MAJOR < 3
    SC_HAS_PROCESS(QueuedTlmPort);
#endif
    QueuedTlmPort(sc_core::sc_module_name name, std::uint64_t ram_bytes)
        : sc_core::sc_module(name), ram_bytes_(ram_bytes) { SC_THREAD(worker); }
    devices::common::Tag submit(devices::common::Master master, devices::common::BusOperation operation,
                                std::uint64_t address, std::size_t count,
                                devices::common::Bytes data = {}) override {
        if (busy_) throw std::length_error("TLM model port busy");
        if (!count || count > 1024 || (operation != devices::common::BusOperation::Read &&
            operation != devices::common::BusOperation::Write) ||
            (operation == devices::common::BusOperation::Write ? data.size() != count : !data.empty()))
            throw std::invalid_argument("invalid queued TLM transaction");
        if (next_tag_ == UINT64_MAX) throw std::overflow_error("TLM tags exhausted");
        const auto tag = next_tag_++;
        if (operation == devices::common::BusOperation::Read) data.resize(count);
        queue_.push_back({tag, generation_, master, operation, address, std::move(data)});
        busy_ = true;
        wake_.notify(sc_core::SC_ZERO_TIME);
        return tag;
    }
    bool ready(devices::common::Tag tag) const override { return results_.count(tag); }
    devices::common::BusResult take(devices::common::Tag tag) override {
        const auto found = results_.find(tag);
        if (found == results_.end()) throw std::out_of_range("TLM response not ready");
        auto result = std::move(found->second);
        results_.erase(found); busy_ = false;
        return result;
    }
    bool is_memory(std::uint32_t address, std::size_t size) const override {
        return size && address < ram_bytes_ && size <= ram_bytes_ - address;
    }
    void reset() {
        ++generation_; queue_.clear(); results_.clear(); busy_ = false;
    }
private:
    struct Job {
        devices::common::Tag tag;
        std::uint64_t generation;
        devices::common::Master master;
        devices::common::BusOperation operation;
        std::uint64_t address;
        devices::common::Bytes data;
    };
    void worker() {
        while (true) {
            if (queue_.empty()) { sc_core::wait(wake_); continue; }
            auto job = std::move(queue_.front()); queue_.pop_front();
            tlm::tlm_generic_payload transaction;
            BusTransaction context;
            context.master = job.master;
            transaction.set_extension(&context);
            struct ExtensionScope {
                tlm::tlm_generic_payload& payload;
                ~ExtensionScope() { payload.clear_extension<BusTransaction>(); }
            } extension_scope{transaction};
            transaction.set_command(job.operation == devices::common::BusOperation::Read
                ? tlm::TLM_READ_COMMAND : tlm::TLM_WRITE_COMMAND);
            transaction.set_address(job.address);
            transaction.set_data_ptr(job.data.data());
            transaction.set_data_length(static_cast<unsigned>(job.data.size()));
            transaction.set_streaming_width(static_cast<unsigned>(job.data.size()));
            auto delay = sc_core::SC_ZERO_TIME;
            socket->b_transport(transaction, delay);
            if (delay != sc_core::SC_ZERO_TIME) sc_core::wait(delay);
            if (job.generation != generation_) continue; // A reset cancelled this request.
            context.result.tag = job.tag;
            results_.emplace(job.tag, std::move(context.result));
        }
    }
    std::uint64_t ram_bytes_, generation_ = 0;
    devices::common::Tag next_tag_ = 1;
    bool busy_ = false;
    sc_core::sc_event wake_;
    std::deque<Job> queue_;
    std::map<devices::common::Tag, devices::common::BusResult> results_;
};

} // namespace colab::simulation::systemc
