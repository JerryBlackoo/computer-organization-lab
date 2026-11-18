#pragma once

#include "address_map.hpp"
#include "cpu16/core.hpp"
#include "rv32/core.hpp"
#include "cpu16/bus.hpp"
#include "controller/interrupt/interrupt.hpp"
#include "controller/pcie/pcie.hpp"
#include "cache/cpu_port.hpp"
#include "controller/memory/dram_timing.hpp"
#include "controller/memory/interleaved_dram_memory.hpp"
#include "../devices/hdd/hdd.hpp"
#include "../devices/nvme/nvme.hpp"
#include "../devices/nic/nic.hpp"
#include <memory>

namespace colab {
enum class DramScheduleMode { Off, LegacySingleIssue, InterleavedPipeline };
struct SoCConfig {
    dimm::DIMMGeometry memory{4, {4, {2, {64, 128, 8}}}}; // 4 x 64 KiB = 256 KiB, 32-bit ranks.
    devices::hdd::Config disk{};
    interconnect::fabric::Config fabric{};
    soc::RV32Core::Mode cpu_mode = soc::RV32Core::Mode::Hardwired;
    soc::RV32Core::Execution cpu_execution = soc::RV32Core::Execution::Pipelined;
    soc::core::pipeline::Options pipeline{};
    bool enable_data_cache = true;
    soc::cache::Config data_cache = soc::cache::CpuPort::default_config();
    soc::core::mmu::TlbConfig tlb{}; // Appended: applies to both CPU translation models.
    unsigned instruction_cache_hit_cycles = 1, data_cache_hit_cycles = 1;
    bool enable_timed_dram = false;
    soc::controller::DramTiming::Config dram_timing{};
    DramScheduleMode dram_mode = DramScheduleMode::Off;
    soc::controller::InterleavedDramScheduler::Config interleaved_dram{};

    DramScheduleMode selected_dram_mode() const {
        switch (dram_mode) {
        case DramScheduleMode::Off: return enable_timed_dram ? DramScheduleMode::LegacySingleIssue : dram_mode;
        case DramScheduleMode::LegacySingleIssue: return dram_mode;
        case DramScheduleMode::InterleavedPipeline:
            if (enable_timed_dram) throw std::invalid_argument("legacy and interleaved DRAM modes are mutually exclusive");
            return dram_mode;
        }
        throw std::invalid_argument("unknown DRAM scheduling mode");
    }
};

// Assemble CPU and devices on a shared interconnect that routes to RAM or MMIO.
// MMIO bypasses Cache; committed RAM writes snoop/update CPU cached copies.
// Instruction ROM remains Harvard-style, as in the existing RV32 core.
class SoC {
public:
    static constexpr std::uint32_t HddBase = soc::address_map::Hdd.base, IrqBase = soc::address_map::Irq.base,
        NvmeBase = soc::address_map::Nvme.base, NicBase = soc::address_map::Nic.base;
    static constexpr unsigned HddMaster = 1, NvmeMaster = 2, NicMaster = 3;
    explicit SoC(std::vector<std::uint32_t> program = {0x00000063}, SoCConfig config = {})
        : dimm_(config.memory), memory_(dimm_), dram_timing_(memory_, config.dram_timing),
          dram_mode_(config.selected_dram_mode()),
          interleaved_dram_(dram_mode_ == DramScheduleMode::InterleavedPipeline
              ? std::make_unique<soc::controller::InterleavedDramMemory>(memory_, 0, config.interleaved_dram) : nullptr),
          bus_(iommu_, config.fabric),
          data_port_(bus_, config.data_cache, config.enable_data_cache, config.data_cache_hit_cycles),
          disk_(config.disk), hdd_(disk_, bus_, HddMaster, irq_, 1),
          nvme_(bus_, NvmeMaster, irq_, 2), nic_(bus_, NicMaster, irq_, 3),
          cpu_(std::move(program), config.cpu_mode, 256, {}, config.cpu_execution, config.pipeline,
               soc::core::FaultHandling::HostException, soc::RV32Core::Translation::Teaching, config.tlb) {
        if (dram_mode_ != DramScheduleMode::Off) {
            interconnect::fabric::Endpoint endpoint{0, config.memory.capacity_bytes(), 2,
                [this](auto address, auto bytes) { return memory_.read(address, bytes); },
                [this](auto address, const auto& bytes) { memory_.write(address, bytes); }, "clocked DIMM"};
            endpoint.prepare = [this](auto tag, auto op, auto address, auto bytes) {
                return interleaved_dram_ ? interleaved_dram_->prepare(tag, op, address, bytes)
                                         : dram_timing_.prepare(tag, op, address, bytes);
            };
            endpoint.release = [this](auto tag, auto address) {
                if (interleaved_dram_) interleaved_dram_->complete(tag, address);
                else dram_timing_.complete(tag, address);
            };
            endpoint.cancel = [this](auto tag, auto address) {
                if (interleaved_dram_) interleaved_dram_->cancel(tag, address);
                else dram_timing_.cancel(tag, address);
            };
            bus_.map_memory_endpoint(std::move(endpoint), config.memory.word_bits() / 8);
        } else bus_.map_memory(0, config.memory.capacity_bytes(), memory_);
        soc::controller::PcieRootComplex pcie_root(bus_);
        pcie_root.attach(HddBase, soc::address_map::Hdd.size, hdd_);
        bus_.map_mmio(IrqBase, soc::address_map::Irq.size, irq_);
        pcie_root.attach(NvmeBase, soc::address_map::Nvme.size, nvme_);
        pcie_root.attach(NicBase, soc::address_map::Nic.size, nic_);
        memory_.set_write_observer([this](auto address, const auto& data) { data_port_.snoop_write(address, data); });
        cpu_.connect_bus(data_port_);
    }
    // Responses/side effects from queued requests, then CPU Cache and device/DMA state,
    // then interrupt sampling and one CPU clock. No requester ticks the fabric.
    // Newly submitted fabric requests wait until at least the next system tick.
    void tick() {
        if (interleaved_dram_) interleaved_dram_->tick();
        else if (dram_mode_ == DramScheduleMode::LegacySingleIssue) dram_timing_.tick();
        bus_.tick();
        data_port_.tick();
        hdd_.tick(); nvme_.tick(); nic_.tick();
        cpu_.interrupts().set_external(irq_.requested());
        cpu_.tick();
        ++cycles_;
    }
    soc::RV32Core& cpu() { return cpu_; }
    soc::controller::Memory& memory() { return memory_; }
    const soc::controller::DramTiming& dram_timing() const { return dram_timing_; }
    const soc::controller::InterleavedDramMemory* interleaved_dram() const { return interleaved_dram_.get(); }
    dimm::DIMM& dimm() { return dimm_; }
    Interconnect& bus() { return bus_; }
    const soc::Cache& data_cache() const { return data_port_.cache(); }
    const soc::cache::CpuPort& data_port() const { return data_port_; }
    soc::controller::IOMMU& iommu() { return iommu_; }
    soc::controller::Interrupt& interrupts() { return irq_; }
    devices::HDD& disk() { return disk_; } // host image/power controls only
    devices::hdd::IOController<devices::HDD>& hdd() { return hdd_; }
    devices::NVMe& nvme() { return nvme_; }
    devices::NIC& nic() { return nic_; }
    std::uint64_t cycles() const { return cycles_; }
private:
    dimm::DIMM dimm_;
    soc::controller::Memory memory_;
    soc::controller::DramTiming dram_timing_;
    DramScheduleMode dram_mode_;
    std::unique_ptr<soc::controller::InterleavedDramMemory> interleaved_dram_;
    soc::controller::IOMMU iommu_;
    Interconnect bus_;
    soc::cache::CpuPort data_port_;
    soc::controller::Interrupt irq_;
    devices::HDD disk_;
    devices::hdd::IOController<devices::HDD> hdd_;
    devices::NVMe nvme_;
    devices::NIC nic_;
    soc::RV32Core cpu_;
    std::uint64_t cycles_ = 0;
};
}
