#pragma once

#include "queued_tlm_port.hpp"
#include "../../../soc/cache/cpu_port.hpp"
#include "../../../soc/core/rv32_core.hpp"
#include "../../../soc/controller/interrupt/interrupt.hpp"
#include "../../../devices/clint/clint.hpp"
#include <functional>
#include <tlm_utils/simple_target_socket.h>

namespace colab::simulation::systemc {

class CacheModule : public sc_core::sc_module, public devices::common::DmaBus {
public:
#if SC_VERSION_MAJOR < 3
    SC_HAS_PROCESS(CacheModule);
#endif
    CacheModule(sc_core::sc_module_name name, const sc_core::sc_event& clock,
                QueuedTlmPort& port, soc::cache::Config config = soc::cache::CpuPort::default_config(), bool enabled = true,
                unsigned hit_cycles = 1)
        : sc_core::sc_module(name), port_(port), config_(config), enabled_(enabled), hit_cycles_(hit_cycles) {
        reset();
        SC_METHOD(on_clock); sensitive << clock; dont_initialize();
    }
    void reset() { snoops_.clear(); hardware_.reset(new soc::cache::CpuPort(port_, config_, enabled_, hit_cycles_)); }
    devices::common::Tag submit(devices::common::Master master, devices::common::BusOperation operation,
                                std::uint64_t address, std::size_t count, devices::common::Bytes data = {}) override {
        return hardware_->submit(master, operation, address, count, std::move(data));
    }
    bool ready(devices::common::Tag tag) const override { return hardware_->ready(tag); }
    devices::common::BusResult take(devices::common::Tag tag) override { return hardware_->take(tag); }
    bool is_memory(std::uint32_t address, std::size_t bytes) const override { return port_.is_memory(address, bytes); }
    void snoop_write(std::uint32_t address, const devices::common::Bytes& bytes) { snoops_.push_back({address, bytes}); }
    const soc::cache::CpuPort& hardware() const { return *hardware_; }
private:
    void on_clock() {
        // Bus notifications are messages; the cache applies them in its own
        // component phase before consuming a potentially concurrent line fill.
        for (const auto& [address, bytes] : snoops_) hardware_->snoop_write(address, bytes);
        snoops_.clear();
        hardware_->tick();
    }
    QueuedTlmPort& port_;
    soc::cache::Config config_;
    bool enabled_;
    unsigned hit_cycles_;
    std::vector<std::pair<std::uint32_t, devices::common::Bytes>> snoops_;
    std::unique_ptr<soc::cache::CpuPort> hardware_;
};

// Device source levels are latched by their owning models and sampled in the
// IRQ phase. MMIO acknowledgement occurs in the earlier bus phase.
class IrqLatch : public devices::common::IrqSink {
public:
    void set_irq(unsigned source, bool asserted) override {
        if (!source || source >= levels.size()) throw std::out_of_range("IRQ source");
        levels[source] = asserted;
    }
    std::array<bool, 32> levels{};
};

class InterruptModule : public sc_core::sc_module {
public:
    sc_core::sc_signal<bool> pending{"pending"};
    soc::controller::Interrupt hardware;
#if SC_VERSION_MAJOR < 3
    SC_HAS_PROCESS(InterruptModule);
#endif
    InterruptModule(sc_core::sc_module_name name, const sc_core::sc_event& clock, IrqLatch& sources)
        : sc_core::sc_module(name), sources_(sources) {
        SC_METHOD(sample); sensitive << clock << refresh_; dont_initialize();
    }
    void reset() { hardware = {}; sources_.levels.fill(false); refresh_.notify(sc_core::SC_ZERO_TIME); }
private:
    void sample() {
        for (unsigned i = 1; i < sources_.levels.size(); ++i) hardware.set_irq(i, sources_.levels[i]);
        pending.write(hardware.requested());
    }
    IrqLatch& sources_;
    sc_core::sc_event refresh_;
};

template<class Model>
class DeviceModule : public sc_core::sc_module, public devices::common::MmioDevice {
public:
    using Factory = std::function<std::unique_ptr<Model>()>;
#if SC_VERSION_MAJOR < 3
    SC_HAS_PROCESS(DeviceModule);
#endif
    DeviceModule(sc_core::sc_module_name name, const sc_core::sc_event& clock, Factory factory)
        : sc_core::sc_module(name), factory_(std::move(factory)) {
        reset();
        SC_METHOD(on_clock); sensitive << clock; dont_initialize();
    }
    void reset() { hardware_ = factory_(); }
    std::uint32_t mmio_read32(std::uint32_t offset) override { return hardware_->mmio_read32(offset); }
    void mmio_write32(std::uint32_t offset, std::uint32_t value) override { hardware_->mmio_write32(offset, value); }
    void tick() override { hardware_->tick(); }
    Model& hardware() { return *hardware_; }
private:
    void on_clock() { tick(); }
    Factory factory_;
    std::unique_ptr<Model> hardware_;
};

class ClintModule : public sc_core::sc_module, public devices::common::MmioDevice {
public:
    sc_core::sc_signal<bool> timer_irq{"timer_irq"}, software_irq{"software_irq"};
    devices::Clint hardware;
#if SC_VERSION_MAJOR < 3
    SC_HAS_PROCESS(ClintModule);
#endif
    ClintModule(sc_core::sc_module_name name, const sc_core::sc_event& clock)
        : sc_core::sc_module(name), clock_(clock) {
        SC_METHOD(update); sensitive << clock_ << refresh_; dont_initialize();
    }
    void reset() { hardware = {}; refresh_.notify(sc_core::SC_ZERO_TIME); }
    std::uint32_t mmio_read32(std::uint32_t offset) override { return hardware.mmio_read32(offset); }
    void mmio_write32(std::uint32_t offset, std::uint32_t value) override { hardware.mmio_write32(offset, value); }
    void tick() override { hardware.tick(); }
private:
    void update() {
        if (clock_.triggered()) tick();
        timer_irq.write(hardware.timer_pending()); software_irq.write(hardware.software_pending());
    }
    const sc_core::sc_event& clock_;
    sc_core::sc_event refresh_;
};

class CpuModule : public sc_core::sc_module {
public:
    sc_core::sc_in<bool> irq{"irq"};
    sc_core::sc_in<bool> timer_irq{"timer_irq"}, software_irq{"software_irq"};
    std::string fault;
#if SC_VERSION_MAJOR < 3
    SC_HAS_PROCESS(CpuModule);
#endif
    CpuModule(sc_core::sc_module_name name, const sc_core::sc_event& clock,
              CacheModule& instructions, CacheModule& data, std::uint32_t entry,
              soc::core::FaultHandling faults = soc::core::FaultHandling::MachineTrap,
              soc::core::mmu::TlbConfig tlb = {})
        : sc_core::sc_module(name), instructions_(instructions), data_(data), entry_(entry), faults_(faults), tlb_(tlb) {
        reset();
        SC_METHOD(on_clock); sensitive << clock; dont_initialize();
    }
    void reset() {
        hardware_.reset(new soc::RV32Core({}, soc::RV32Core::Mode::Hardwired, 256, {},
            soc::RV32Core::Execution::MultiCycle, {}, faults_, soc::RV32Core::Translation::Architectural, tlb_));
        hardware_->connect_bus(data_);
        hardware_->connect_instruction_bus(instructions_);
        hardware_->set_boot_pc(entry_);
        fault.clear();
    }
    soc::RV32Core& hardware() { return *hardware_; }
private:
    void on_clock() {
        if (!fault.empty()) return;
        try {
            hardware_->interrupts().set_external(irq.read());
            hardware_->interrupts().set_timer(timer_irq.read());
            hardware_->interrupts().set_software(software_irq.read());
            hardware_->tick();
        } catch (const soc::core::RV32Fault& error) {
            fault = "CPU fault cause=" + std::to_string(static_cast<unsigned>(error.cause())) +
                " pc=" + std::to_string(error.pc()) + " value=" + std::to_string(error.value());
        } catch (const std::exception& error) { fault = error.what(); }
    }
    CacheModule& instructions_;
    CacheModule& data_;
    std::uint32_t entry_;
    soc::core::FaultHandling faults_;
    soc::core::mmu::TlbConfig tlb_;
    std::unique_ptr<soc::RV32Core> hardware_;
};

// Untimed MMIO endpoint: the outer fabric already accounts for bus latency.
class MmioTarget : public sc_core::sc_module {
public:
    tlm_utils::simple_target_socket<MmioTarget> socket{"socket"};
    MmioTarget(sc_core::sc_module_name name, devices::common::MmioDevice& device, std::uint32_t base)
        : sc_core::sc_module(name), device_(device), base_(base) {
        socket.register_b_transport(this, &MmioTarget::transport);
    }
private:
    void transport(tlm::tlm_generic_payload& transaction, sc_core::sc_time& delay) {
        transaction.set_dmi_allowed(false);
        if (delay != sc_core::SC_ZERO_TIME || transaction.get_data_length() != 4 ||
            transaction.get_address() % 4 || transaction.get_address() < base_ ||
            !transaction.get_data_ptr() || transaction.get_byte_enable_ptr() || transaction.get_streaming_width() < 4) {
            transaction.set_response_status(tlm::TLM_BURST_ERROR_RESPONSE); return;
        }
        const auto offset = static_cast<std::uint32_t>(transaction.get_address() - base_);
        try {
            if (transaction.is_read()) {
                const auto bytes = devices::common::word_bytes(device_.mmio_read32(offset));
                std::copy(bytes.begin(), bytes.end(), transaction.get_data_ptr());
            } else if (transaction.is_write()) {
                const auto* begin = transaction.get_data_ptr();
                device_.mmio_write32(offset, devices::common::load32({begin, begin + 4}));
            } else { transaction.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE); return; }
            transaction.set_response_status(tlm::TLM_OK_RESPONSE);
        } catch (const std::exception&) { transaction.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE); }
    }
    devices::common::MmioDevice& device_;
    std::uint32_t base_;
};

} // namespace colab::simulation::systemc
