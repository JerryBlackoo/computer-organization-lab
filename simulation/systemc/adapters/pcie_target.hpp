#pragma once

#include "../bus/pcie_request.hpp"
#include <algorithm>
#include <tlm_utils/simple_target_socket.h>

namespace colab::simulation::systemc {

class PcieTarget : public sc_core::sc_module {
public:
    tlm_utils::simple_target_socket<PcieTarget> socket{"socket"};

    PcieTarget(sc_core::sc_module_name name, devices::common::MmioDevice& registers,
               std::uint32_t base, std::uint64_t size)
        : sc_core::sc_module(name), endpoint_(base, size, registers) {
        socket.register_b_transport(this, &PcieTarget::transport);
    }

private:
    void transport(tlm::tlm_generic_payload& transaction, sc_core::sc_time& delay) {
        transaction.set_dmi_allowed(false);
        const auto* packet = transaction.get_extension<PcieRequest>();
        if (!packet || delay != sc_core::SC_ZERO_TIME) {
            transaction.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
            return;
        }
        try {
            if (const auto* write = std::get_if<pcie::MemoryWrite>(&packet->request)) {
                endpoint_.receive(*write);
            } else {
                if (!transaction.get_data_ptr() || transaction.get_data_length() != 4) {
                    transaction.set_response_status(tlm::TLM_BURST_ERROR_RESPONSE);
                    return;
                }
                const auto data = devices::common::word_bytes(endpoint_.receive(std::get<pcie::MemoryRead>(packet->request)));
                std::copy(data.begin(), data.end(), transaction.get_data_ptr());
            }
            transaction.set_response_status(tlm::TLM_OK_RESPONSE);
        } catch (const std::exception&) {
            transaction.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
        }
    }

    pcie::BarEndpoint endpoint_;
};

} // namespace colab::simulation::systemc
