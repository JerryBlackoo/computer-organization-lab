#pragma once

#include "platform/phase_clock.hpp"
#include "adapters/clocked_models.hpp"
#include "adapters/memory_target.hpp"
#include "bus/tlm_fabric.hpp"
#include "../../soc/soc.hpp"
#include "../../devices/timer/timer.hpp"
#include "../../devices/uart/uart.hpp"
#include "../boot/elf32.hpp"

namespace colab::simulation::systemc {

struct BootImage {
    std::uint32_t entry = 0;
    std::vector<std::uint32_t> text;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> data;
    std::vector<boot::Segment> segments{};
};

// Real model platform: no SoC::tick, no host-side instruction interpretation.
// Modules own their state; transactions travel over the model queue -> TLM ->
// existing timed fabric/IOMMU -> TLM target -> hardware-model path.
class Platform : public sc_core::sc_module {
public:
    static constexpr std::uint32_t TimerBase = 0x40000000;
    static constexpr std::uint32_t UartBase = 0x40001000;
    static constexpr std::uint32_t ClintBase = 0x02000000;
    sc_core::sc_signal<std::uint64_t> cycles{"cycles"}, retired{"retired"}, irq_entries{"irq_entries"};
    sc_core::sc_signal<std::uint32_t> pc{"pc"}, bus_master{"bus_master"}, bus_phase{"bus_phase"}, timer_count{"timer_count"};
    sc_core::sc_signal<bool> irq_pending{"irq_pending"}, bus_request{"bus_request"}, bus_ack{"bus_ack"}, timer_pending{"timer_pending"};
    sc_core::sc_signal<bool> uart_rx_ready{"uart_rx_ready"}, uart_tx_ready{"uart_tx_ready"};
    sc_core::sc_signal<std::uint64_t> nvme_commands{"nvme_commands"}, hdd_dma_bytes{"hdd_dma_bytes"};
    sc_core::sc_signal<std::uint64_t> exception_entries{"exception_entries"};
    sc_core::sc_signal<std::uint32_t> mcause{"mcause"}, mepc{"mepc"}, mtval{"mtval"};
    sc_core::sc_signal<std::uint32_t> scause{"scause"}, sepc{"sepc"}, stval{"stval"};
    sc_core::sc_signal<bool> synchronous_trap{"synchronous_trap"};
    sc_core::sc_signal<std::uint32_t> satp{"satp"};
    sc_core::sc_signal<std::uint64_t> page_walks{"page_walks"}, page_table_reads{"page_table_reads"}, page_faults{"page_faults"};
    sc_core::sc_signal<std::uint32_t> privilege{"privilege"};
    sc_core::sc_signal<bool> cpu_waiting{"cpu_waiting"}, mtip{"mtip"}, msip{"msip"};
    sc_core::sc_signal<std::uint64_t> mtime{"mtime"}, mtimecmp{"mtimecmp"}, wfi_wait_cycles{"wfi_wait_cycles"};
    using HddController = devices::hdd::IOController<devices::HDD>;
#if SC_VERSION_MAJOR < 3
    SC_HAS_PROCESS(Platform);
#endif
    Platform(sc_core::sc_module_name name, BootImage image,
             sc_core::sc_time period = sc_core::sc_time(50, sc_core::SC_US), SoCConfig config = {},
             soc::core::FaultHandling faults = soc::core::FaultHandling::MachineTrap)
        : sc_core::sc_module(name), image_(std::move(image)), period_(period),
          config_(clock_config(std::move(config), period)),
          clock_("clock", period_, 0.5, period_, true), phases_("clock_control"),
          dimm_(config_.memory), memory_(dimm_),
          instruction_link_("instruction_tlm", config_.memory.capacity_bytes()),
          data_link_("data_tlm", config_.memory.capacity_bytes()),
          hdd_link_("hdd_dma_tlm", config_.memory.capacity_bytes()),
          nvme_link_("nvme_dma_tlm", config_.memory.capacity_bytes()),
          nic_link_("nic_dma_tlm", config_.memory.capacity_bytes()),
          host_link_("debug_tlm", config_.memory.capacity_bytes()),
          instructions_("instruction_cache", phases_.components, instruction_link_),
          data_("data_cache", phases_.components, data_link_, config_.data_cache, config_.enable_data_cache),
          disk_(config_.disk),
          hdd_("hdd", phases_.components, [this] {
              return std::make_unique<HddController>(disk_, hdd_link_, SoC::HddMaster, sources_, 1);
          }),
          nvme_("nvme", phases_.components, [this] {
              return std::make_unique<devices::nvme::Controller>(nand_, nvme_link_, SoC::NvmeMaster, sources_, 2);
          }),
          nic_("nic", phases_.components, [this] {
              return std::make_unique<devices::NIC>(nic_link_, SoC::NicMaster, sources_, 3);
          }),
          timer_("timer", phases_.components, [this] {
              return std::make_unique<devices::Timer>(sources_, 4);
          }),
          uart_("uart", phases_.components, [this] {
              return std::make_unique<devices::Uart>(sources_, 5);
          }),
          clint_("clint", phases_.components),
          interrupts_("interrupt_controller", phases_.interrupts, sources_),
          cpu_("cpu", phases_.cpu, instructions_, data_, image_.entry, faults),
          ram_target_("dram", memory_, sc_core::SC_ZERO_TIME),
          hdd_target_("hdd_registers", hdd_, SoC::HddBase),
          nvme_target_("nvme_registers", nvme_, SoC::NvmeBase),
          nic_target_("nic_registers", nic_, SoC::NicBase),
          irq_target_("irq_registers", interrupts_.hardware, SoC::IrqBase),
          timer_target_("timer_registers", timer_, TimerBase),
          uart_target_("uart_registers", uart_, UartBase),
          clint_target_("clint_registers", clint_, ClintBase),
          fabric_("pcie", phases_.bus, iommu_, config_.fabric) {
        phases_.clk(clock_);
        cpu_.irq(interrupts_.pending);
        cpu_.timer_irq(clint_.timer_irq); cpu_.software_irq(clint_.software_irq);
        for (auto* port : ports()) port->socket.bind(fabric_.upstream);
        fabric_.downstream.bind(ram_target_.socket);
        fabric_.map(0, config_.memory.capacity_bytes(), 2, true);
        fabric_.downstream.bind(hdd_target_.socket); fabric_.map(SoC::HddBase, 0x100, 1, false);
        fabric_.downstream.bind(nvme_target_.socket); fabric_.map(SoC::NvmeBase, 0x2000, 1, false);
        fabric_.downstream.bind(nic_target_.socket); fabric_.map(SoC::NicBase, 0x100, 1, false);
        fabric_.downstream.bind(irq_target_.socket); fabric_.map(SoC::IrqBase, 0x100, 1, false);
        fabric_.downstream.bind(timer_target_.socket); fabric_.map(TimerBase, 0x100, 1, false);
        fabric_.downstream.bind(uart_target_.socket); fabric_.map(UartBase, 0x100, 1, false);
        fabric_.downstream.bind(clint_target_.socket); fabric_.map(ClintBase, 0xc000, 1, false);
        memory_.set_write_observer([this](auto address, const auto& bytes) {
            instructions_.snoop_write(address, bytes); data_.snoop_write(address, bytes);
        });
        validate_image();
        SC_METHOD(snapshot);
        sensitive << phases_.observe << refresh_;
        dont_initialize();
    }

    // Host controls are invoked only while sc_start is suspended, on its thread.
    void power_on() {
        if (powered_) throw std::logic_error("machine already powered on");
        powered_ = true;
        phases_.power_good.write(true); phases_.reset_asserted.write(true);
        reset_models();
        memory_.write(0, devices::common::Bytes(config_.memory.capacity_bytes(), 0));
        for (std::size_t i = 0; i < image_.text.size(); ++i)
            memory_.write(image_.entry + static_cast<std::uint32_t>(i * 4), devices::common::word_bytes(image_.text[i]));
        for (const auto& [address, value] : image_.data) memory_.write(address, devices::common::word_bytes(value));
        for (const auto& segment : image_.segments) memory_.write(segment.address, segment.data);
        release_reset();
    }
    void power_off() {
        phases_.remaining = 0; powered_ = false;
        phases_.power_good.write(false); phases_.reset_asserted.write(true);
        reset_models(); disk_.power_off(); settle();
    }
    void reset() {
        require_power();
        phases_.remaining = 0; phases_.reset_asserted.write(true);
        reset_models(); release_reset(); // Preserve RAM, NAND, and magnetic media.
    }
    void pause() { phases_.remaining = 0; }
    void step() { run(1); }
    void run(std::uint64_t count) {
        require_power();
        if (!count) return;
        phases_.remaining = count;
        while (phases_.remaining) sc_core::sc_start(period_ * static_cast<double>(phases_.remaining));
        // Timed sc_start already settles every executed edge. Do not consume
        // the next clock edge with a zero-time start after disabling its budget.
    }
    bool powered() const { return powered_; }
    const std::string& fault() const { return cpu_.fault; }
    soc::RV32Core& cpu() { return cpu_.hardware(); }
    soc::controller::Memory& memory() { return memory_; }
    soc::controller::IOMMU& iommu() { return iommu_; }
    devices::nvme::NAND& nand() { return nand_; }
    devices::HDD& disk() { return disk_; }
    devices::nvme::Controller& nvme() { return nvme_.hardware(); }
    devices::NIC& nic() { return nic_.hardware(); }
    devices::Clint& clint() { return clint_.hardware; }
    double timebase_hz() const { return 1.0 / period_.to_seconds(); }
    HddController& hdd() { return hdd_.hardware(); }
    devices::Timer& timer() { return timer_.hardware(); }
    devices::Uart& uart() { return uart_.hardware(); }
    const soc::cache::CpuPort& instruction_cache() const { return instructions_.hardware(); }
    const soc::cache::CpuPort& data_cache() const { return data_.hardware(); }
    const Interconnect& bus() const { return fabric_.hardware(); }
    QueuedTlmPort& debug_port() { return host_link_; }

    void trace(sc_core::sc_trace_file* file) const {
        sc_core::sc_trace(file, clock_, "clock");
        sc_core::sc_trace(file, phases_.power_good, "power_good");
        sc_core::sc_trace(file, phases_.reset_asserted, "reset");
        sc_core::sc_trace(file, cycles, "cycles"); sc_core::sc_trace(file, pc, "pc");
        sc_core::sc_trace(file, retired, "retired"); sc_core::sc_trace(file, irq_pending, "irq_pending");
        sc_core::sc_trace(file, irq_entries, "irq_entries");
        sc_core::sc_trace(file, exception_entries, "exception_entries");
        sc_core::sc_trace(file, synchronous_trap, "synchronous_trap");
        sc_core::sc_trace(file, satp, "satp"); sc_core::sc_trace(file, page_walks, "page_walks");
        sc_core::sc_trace(file, page_table_reads, "page_table_reads"); sc_core::sc_trace(file, page_faults, "page_faults");
        sc_core::sc_trace(file, privilege, "privilege");
        sc_core::sc_trace(file, cpu_waiting, "cpu_waiting"); sc_core::sc_trace(file, wfi_wait_cycles, "wfi_wait_cycles");
        sc_core::sc_trace(file, mtime, "mtime"); sc_core::sc_trace(file, mtimecmp, "mtimecmp");
        sc_core::sc_trace(file, mtip, "mtip"); sc_core::sc_trace(file, msip, "msip");
        sc_core::sc_trace(file, mcause, "mcause"); sc_core::sc_trace(file, mepc, "mepc"); sc_core::sc_trace(file, mtval, "mtval");
        sc_core::sc_trace(file, scause, "scause"); sc_core::sc_trace(file, sepc, "sepc"); sc_core::sc_trace(file, stval, "stval");
        sc_core::sc_trace(file, bus_master, "bus_master"); sc_core::sc_trace(file, bus_phase, "bus_phase");
        sc_core::sc_trace(file, bus_request, "bus_request"); sc_core::sc_trace(file, bus_ack, "bus_ack");
        sc_core::sc_trace(file, nvme_commands, "nvme_commands"); sc_core::sc_trace(file, hdd_dma_bytes, "hdd_dma_bytes");
        sc_core::sc_trace(file, timer_count, "timer_count"); sc_core::sc_trace(file, timer_pending, "timer_pending");
        sc_core::sc_trace(file, uart_rx_ready, "uart_rx_ready"); sc_core::sc_trace(file, uart_tx_ready, "uart_tx_ready");
    }
private:
    static SoCConfig clock_config(SoCConfig config, sc_core::sc_time period) {
        if (period == sc_core::SC_ZERO_TIME) throw std::invalid_argument("positive system clock period required");
        config.disk.tick_ms = period.to_seconds() * 1000.0L;
        return config;
    }
    std::array<QueuedTlmPort*, 6> ports() {
        return {&instruction_link_, &data_link_, &hdd_link_, &nvme_link_, &nic_link_, &host_link_};
    }
    void require_power() const { if (!powered_) throw std::logic_error("Power On before running the machine"); }
    void validate_image() const {
        const auto capacity = config_.memory.capacity_bytes();
        if (!image_.segments.empty()) {
            if (!image_.text.empty() || !image_.data.empty() || image_.entry % 4)
                throw std::invalid_argument("cannot mix ELF segments with legacy boot image");
            bool executable_entry = false;
            for (std::size_t i = 0; i < image_.segments.size(); ++i) {
                const auto& segment = image_.segments[i];
                const auto end = std::uint64_t(segment.address) + segment.memory_size;
                if (!segment.memory_size || segment.data.size() > segment.memory_size || end > capacity)
                    throw std::invalid_argument("invalid boot segment range");
                for (std::size_t j = 0; j < i; ++j) {
                    const auto& prior = image_.segments[j];
                    if (segment.address < std::uint64_t(prior.address) + prior.memory_size && prior.address < end)
                        throw std::invalid_argument("overlapping boot segments");
                }
                if ((segment.flags & 1) && image_.entry >= segment.address &&
                    std::uint64_t(image_.entry) + 4 <= std::uint64_t(segment.address) + segment.data.size())
                    executable_entry = true;
            }
            if (!executable_entry) throw std::invalid_argument("boot entry is outside executable data");
            return;
        }
        if (image_.text.empty() || image_.entry % 4 || image_.entry >= capacity ||
            image_.text.size() > (capacity - image_.entry) / 4)
            throw std::invalid_argument("boot text is outside DIMM");
        const auto text_end = image_.entry + image_.text.size() * 4;
        for (const auto& [address, value] : image_.data) {
            (void)value;
            if (address % 4 || address >= capacity || capacity - address < 4 ||
                (address < text_end && std::uint64_t(address) + 4 > image_.entry))
                throw std::invalid_argument("boot data overlaps text or lies outside DIMM");
        }
    }
    void reset_models() {
        fabric_.reset();
        for (auto* port : ports()) port->reset();
        instructions_.reset(); data_.reset();
        disk_.power_off();
        while (disk_.take_completion()) {}
        hdd_.reset(); nvme_.reset(); nic_.reset(); timer_.reset(); uart_.reset(); clint_.reset(); interrupts_.reset(); cpu_.reset();
        iommu_ = {};
        phases_.cycles = 0;
        refresh_.notify(sc_core::SC_ZERO_TIME);
    }
    void release_reset() {
        disk_.power_on();
        settle();
        sc_core::sc_start(period_ * 2.0); // Powered clock edges while CPU remains in reset.
        phases_.reset_asserted.write(false);
        refresh_.notify(sc_core::SC_ZERO_TIME);
        settle();
    }
    static void settle() {
        sc_core::sc_start(sc_core::SC_ZERO_TIME);
        unsigned deltas = 0;
        while (sc_core::sc_pending_activity_at_current_time()) {
            if (++deltas > 1000) throw std::runtime_error("SystemC did not settle");
            sc_core::sc_start(sc_core::SC_ZERO_TIME);
        }
    }
    void snapshot() {
        cycles.write(phases_.cycles); pc.write(cpu().pc()); retired.write(cpu().completed_instructions());
        irq_pending.write(interrupts_.hardware.requested()); irq_entries.write(cpu().interrupts().entries());
        exception_entries.write(cpu().interrupts().exceptions());
        privilege.write(static_cast<std::uint32_t>(cpu().interrupts().privilege()));
        satp.write(cpu().interrupts().read(0x180));
        page_walks.write(cpu().sv32().statistics().walks); page_table_reads.write(cpu().sv32().statistics().reads);
        page_faults.write(cpu().sv32().statistics().page_faults);
        cpu_waiting.write(cpu().waiting()); wfi_wait_cycles.write(cpu().wfi_wait_cycles());
        mtime.write(clint().time()); mtimecmp.write(clint().compare());
        mtip.write(clint().timer_pending()); msip.write(clint().software_pending());
        synchronous_trap.write(cpu().last_cycle() && cpu().last_cycle()->trap.has_value());
        mcause.write(cpu().interrupts().read(0x342)); mepc.write(cpu().interrupts().read(0x341));
        mtval.write(cpu().interrupts().read(0x343));
        scause.write(cpu().interrupts().read(0x142)); sepc.write(cpu().interrupts().read(0x141));
        stval.write(cpu().interrupts().read(0x143));
        nvme_commands.write(nvme().completed_commands()); hdd_dma_bytes.write(hdd().dma().transferred());
        timer_count.write(timer().mmio_read32(devices::Timer::Count));
        timer_pending.write(timer().mmio_read32(devices::Timer::Pending) != 0);
        const auto uart_status = uart().mmio_read32(devices::Uart::Status);
        uart_rx_ready.write((uart_status & devices::Uart::RxReady) != 0);
        uart_tx_ready.write((uart_status & devices::Uart::TxReady) != 0);
        const auto& tick = bus().fabric().last_tick();
        bus_master.write(tick.master.value_or(UINT32_MAX)); bus_phase.write(static_cast<std::uint32_t>(tick.phase));
        bus_request.write(tick.req); bus_ack.write(tick.ack);
    }
    BootImage image_;
    sc_core::sc_time period_;
    SoCConfig config_;
    bool powered_ = false;
    sc_core::sc_clock clock_;
    PhaseClock phases_;
    dimm::DIMM dimm_;
    soc::controller::Memory memory_;
    soc::controller::IOMMU iommu_;
    IrqLatch sources_;
    QueuedTlmPort instruction_link_, data_link_, hdd_link_, nvme_link_, nic_link_, host_link_;
    CacheModule instructions_, data_;
    devices::nvme::NAND nand_;
    devices::HDD disk_;
    DeviceModule<HddController> hdd_;
    DeviceModule<devices::nvme::Controller> nvme_;
    DeviceModule<devices::NIC> nic_;
    DeviceModule<devices::Timer> timer_;
    DeviceModule<devices::Uart> uart_;
    ClintModule clint_;
    InterruptModule interrupts_;
    CpuModule cpu_;
    MemoryTarget ram_target_;
    MmioTarget hdd_target_, nvme_target_, nic_target_, irq_target_, timer_target_, uart_target_, clint_target_;
    TlmFabric fabric_;
    sc_core::sc_event refresh_;
};

} // namespace colab::simulation::systemc
