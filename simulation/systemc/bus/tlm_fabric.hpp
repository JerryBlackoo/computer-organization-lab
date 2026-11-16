#pragma once

#include "../adapters/queued_tlm_port.hpp"
#include "../../../interconnect/interconnect.hpp"
#include <tlm_utils/multi_passthrough_initiator_socket.h>
#include <tlm_utils/multi_passthrough_target_socket.h>

namespace colab::simulation::systemc {

// Reuse the existing round-robin timed fabric and IOMMU. Every backend endpoint
// is reached over TLM; its delay is already modeled by the fabric's phases.
class TlmFabric : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<TlmFabric> upstream{"upstream"};
    tlm_utils::multi_passthrough_initiator_socket<TlmFabric> downstream{"downstream"};
#if SC_VERSION_MAJOR < 3
    SC_HAS_PROCESS(TlmFabric);
#endif
    TlmFabric(sc_core::sc_module_name name, const sc_core::sc_event& clock,
              soc::controller::IOMMU& iommu, interconnect::pcie::Config config = {})
        : sc_core::sc_module(name), iommu_(iommu), config_(config), hardware_(new Interconnect(iommu, config)) {
        upstream.register_b_transport(this, &TlmFabric::transport);
        SC_METHOD(on_clock);
        sensitive << clock;
        dont_initialize();
    }
    void map(std::uint32_t base, std::uint64_t size, unsigned latency, bool ram) {
        windows_.push_back({base, size, latency, ram});
        attach(windows_.size() - 1);
    }
    void reset() {
        ++generation_;
        for (auto& [tag, pending] : pending_) {
            pending->result = {tag, 0, interconnect::pcie::Status::Timeout, {}, 0, "platform reset", false};
            pending->done.notify(sc_core::SC_ZERO_TIME);
        }
        pending_.clear();
        advanced_.notify(sc_core::SC_ZERO_TIME);
        hardware_.reset(new Interconnect(iommu_, config_));
        for (std::size_t i = 0; i < windows_.size(); ++i) attach(i);
    }
    const Interconnect& hardware() const { return *hardware_; }
private:
    struct Window { std::uint32_t base; std::uint64_t size; unsigned latency; bool ram; };
    struct Pending { sc_core::sc_event done; devices::common::BusResult result{}; };
    void attach(std::size_t index) {
        const auto window = windows_.at(index);
        interconnect::pcie::Endpoint endpoint{window.base, window.size, window.latency,
            [this, index](auto address, auto count) { return forward(index, address, devices::common::Bytes(count), false); },
            [this, index](auto address, const auto& data) { forward(index, address, data, true); }, "TLM endpoint"};
        if (window.ram) hardware_->map_memory_endpoint(std::move(endpoint));
        else hardware_->map_mmio_endpoint(std::move(endpoint));
    }
    devices::common::Bytes forward(std::size_t index, std::uint32_t address,
                                   devices::common::Bytes bytes, bool write) {
        tlm::tlm_generic_payload transaction;
        transaction.set_command(write ? tlm::TLM_WRITE_COMMAND : tlm::TLM_READ_COMMAND);
        transaction.set_address(address);
        transaction.set_data_ptr(bytes.data());
        transaction.set_data_length(static_cast<unsigned>(bytes.size()));
        transaction.set_streaming_width(static_cast<unsigned>(bytes.size()));
        auto delay = sc_core::SC_ZERO_TIME;
        downstream[static_cast<int>(index)]->b_transport(transaction, delay);
        if (delay != sc_core::SC_ZERO_TIME || transaction.is_response_error())
            throw std::runtime_error("TLM endpoint rejected transaction or added an unclocked delay");
        return bytes;
    }
    void transport(int, tlm::tlm_generic_payload& transaction, sc_core::sc_time& delay) {
        auto* context = transaction.get_extension<BusTransaction>();
        if (!context) { transaction.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE); return; }
        transaction.set_dmi_allowed(false);
        if (delay != sc_core::SC_ZERO_TIME) { sc_core::wait(delay); delay = sc_core::SC_ZERO_TIME; }
        const bool write = transaction.get_command() == tlm::TLM_WRITE_COMMAND;
        Pending pending;
        devices::common::Tag tag;
        const auto generation = generation_;
        try {
            if ((!write && transaction.get_command() != tlm::TLM_READ_COMMAND) ||
                !transaction.get_data_ptr() || !transaction.get_data_length() || transaction.get_byte_enable_ptr() ||
                transaction.get_streaming_width() < transaction.get_data_length())
                throw std::invalid_argument("unsupported TLM transaction geometry");
            while (true) {
                if (generation != generation_) throw std::runtime_error("platform reset cancelled queued request");
                try {
                    const auto* begin = transaction.get_data_ptr();
                    tag = hardware_->submit(context->master, write ? devices::common::BusOperation::Write
                        : devices::common::BusOperation::Read, transaction.get_address(), transaction.get_data_length(),
                        write ? devices::common::Bytes(begin, begin + transaction.get_data_length()) : devices::common::Bytes{});
                    break;
                } catch (const std::length_error&) { sc_core::wait(advanced_); }
            }
            pending_.emplace(tag, &pending);
            sc_core::wait(pending.done);
            context->result = std::move(pending.result);
            if (!write) std::copy(context->result.data.begin(), context->result.data.end(), transaction.get_data_ptr());
            transaction.set_response_status(context->result.status == interconnect::pcie::Status::Success
                ? tlm::TLM_OK_RESPONSE : tlm::TLM_GENERIC_ERROR_RESPONSE);
        } catch (const sc_core::sc_unwind_exception&) {
            throw;
        } catch (const std::exception& error) {
            context->result = {0, context->master, interconnect::pcie::Status::BackendError, {}, 0, error.what(), false};
            transaction.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
        }
    }
    void on_clock() {
        hardware_->tick();
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (!hardware_->ready(it->first)) { ++it; continue; }
            it->second->result = hardware_->take(it->first);
            it->second->done.notify(sc_core::SC_ZERO_TIME);
            it = pending_.erase(it);
        }
        advanced_.notify(sc_core::SC_ZERO_TIME);
    }
    soc::controller::IOMMU& iommu_;
    interconnect::pcie::Config config_;
    std::unique_ptr<Interconnect> hardware_;
    std::vector<Window> windows_;
    std::map<devices::common::Tag, Pending*> pending_;
    sc_core::sc_event advanced_;
    std::uint64_t generation_ = 0;
};

} // namespace colab::simulation::systemc
