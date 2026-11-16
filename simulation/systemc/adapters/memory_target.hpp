#pragma once

#include "../../../soc/controller/memory/memory.hpp"
#include <systemc>
#include <tlm>
#include <tlm_utils/simple_target_socket.h>

namespace colab::simulation::systemc {

// Timed memory port. Nonzero latency/delay requires a process allowed to wait;
// zero latency and zero input delay also support an already-clocked fabric.
// Each accepted transaction commits after its latency, never at arrival time.
class MemoryTarget : public sc_core::sc_module {
public:
    tlm_utils::simple_target_socket<MemoryTarget> socket{"socket"};

    MemoryTarget(sc_core::sc_module_name name, soc::controller::Memory& memory,
                 sc_core::sc_time latency)
        : sc_core::sc_module(name), memory_(memory), latency_(latency) {
        socket.register_b_transport(this, &MemoryTarget::transport);
    }

private:
    void transport(tlm::tlm_generic_payload& transaction, sc_core::sc_time& delay) {
        transaction.set_dmi_allowed(false);
        transaction.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
        const auto command = transaction.get_command();
        if (command != tlm::TLM_READ_COMMAND && command != tlm::TLM_WRITE_COMMAND) {
            transaction.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);
            return;
        }
        if (transaction.get_byte_enable_ptr()) {
            transaction.set_response_status(tlm::TLM_BYTE_ENABLE_ERROR_RESPONSE);
            return;
        }
        const auto size = transaction.get_data_length();
        if (!size || transaction.get_streaming_width() < size) {
            transaction.set_response_status(tlm::TLM_BURST_ERROR_RESPONSE);
            return;
        }
        if (!transaction.get_data_ptr()) {
            transaction.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
            return;
        }
        const auto address = transaction.get_address();
        constexpr auto max_address = std::numeric_limits<std::uint32_t>::max();
        if (address > max_address || size - 1 > max_address - address) {
            transaction.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);
            return;
        }

        // Consume incoming annotated delay exactly once, then the target latency.
        const auto duration = delay + latency_;
        if (duration != sc_core::SC_ZERO_TIME) sc_core::wait(duration);
        delay = sc_core::SC_ZERO_TIME;
        try {
            const auto physical = static_cast<std::uint32_t>(address);
            if (command == tlm::TLM_READ_COMMAND) {
                const auto data = memory_.read(physical, size);
                std::copy(data.begin(), data.end(), transaction.get_data_ptr());
            } else {
                const auto* begin = transaction.get_data_ptr();
                memory_.write(physical, {begin, begin + size});
            }
            transaction.set_response_status(tlm::TLM_OK_RESPONSE);
        } catch (const std::out_of_range&) {
            transaction.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);
        }
    }

    soc::controller::Memory& memory_;
    sc_core::sc_time latency_;
};

} // namespace colab::simulation::systemc
