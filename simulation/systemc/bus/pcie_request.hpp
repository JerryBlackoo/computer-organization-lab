#pragma once

#include "../../../interconnect/pcie/endpoint.hpp"
#include <tlm>
#include <variant>

namespace colab::simulation::systemc {

// Carry the decoded transaction type across the target socket.
struct PcieRequest : tlm::tlm_extension<PcieRequest> {
    using Request = std::variant<pcie::MemoryRead, pcie::MemoryWrite>;

    explicit PcieRequest(Request transaction) : request(transaction) {}
    tlm::tlm_extension_base* clone() const override { return new PcieRequest(*this); }
    void copy_from(const tlm::tlm_extension_base& other) override {
        request = static_cast<const PcieRequest&>(other).request;
    }

    Request request;
};

} // namespace colab::simulation::systemc
