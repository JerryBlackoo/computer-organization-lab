#include "../../simulation/systemc/adapters/memory_target.hpp"
#include <tlm_utils/simple_initiator_socket.h>
#include <array>
#include <iostream>

class MemoryInitiator : public sc_core::sc_module {
public:
    tlm_utils::simple_initiator_socket<MemoryInitiator> socket{"socket"};
    bool passed = false;
#if SC_VERSION_MAJOR < 3
    SC_HAS_PROCESS(MemoryInitiator);
#endif
    MemoryInitiator(sc_core::sc_module_name name, std::size_t capacity) : sc_core::sc_module(name), capacity_(capacity) {
        SC_THREAD(run);
    }
private:
    std::size_t capacity_;
    tlm::tlm_response_status transfer(tlm::tlm_command command, std::uint64_t address,
                                     std::array<unsigned char, 4>& data, sc_core::sc_time delay) {
        tlm::tlm_generic_payload transaction;
        transaction.set_command(command);
        transaction.set_address(address);
        transaction.set_data_ptr(data.data());
        transaction.set_data_length(data.size());
        transaction.set_streaming_width(data.size());
        socket->b_transport(transaction, delay);
        if (delay != sc_core::SC_ZERO_TIME) throw std::runtime_error("target did not consume delay");
        return transaction.get_response_status();
    }
    void run() {
        using namespace sc_core;
        const std::array<unsigned char, 4> expected{0x12, 0x34, 0x56, 0x78};
        auto data = expected;
        // Incoming 5ns plus the target's 20ns, consumed by b_transport.
        if (transfer(tlm::TLM_WRITE_COMMAND, 0x100, data, sc_time(5, SC_NS)) != tlm::TLM_OK_RESPONSE ||
            sc_time_stamp() != sc_time(25, SC_NS)) throw std::runtime_error("TLM write timing failed");
        data.fill(0);
        if (transfer(tlm::TLM_READ_COMMAND, 0x100, data, SC_ZERO_TIME) != tlm::TLM_OK_RESPONSE ||
            data != expected || sc_time_stamp() != sc_time(45, SC_NS))
            throw std::runtime_error("TLM read data/timing failed");
        // Out-of-window transfers report a bus error instead of aliasing RAM.
        if (transfer(tlm::TLM_READ_COMMAND, capacity_, data, SC_ZERO_TIME) != tlm::TLM_ADDRESS_ERROR_RESPONSE)
            throw std::runtime_error("TLM address error was lost");
        passed = true;
        std::cout << "PASS TLM: write/read DIMM, delay consumed once, address error; time="
                  << sc_time_stamp() << '\n';
        sc_stop();
    }
};

int sc_main(int, char**) {
    colab::dimm::DIMM dimm({4, {4, {2, {64, 128, 8}}}});
    colab::soc::controller::Memory memory(dimm);
    colab::simulation::systemc::MemoryTarget target("memory", memory, sc_core::sc_time(20, sc_core::SC_NS));
    MemoryInitiator initiator("initiator", dimm.geometry().capacity_bytes());
    initiator.socket.bind(target.socket);
    sc_core::sc_start(1, sc_core::SC_US);
    return initiator.passed ? 0 : 1;
}
