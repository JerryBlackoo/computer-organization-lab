#pragma once

#include "core/core.hpp"
#include "core/rv32_core.hpp"
#include "noc/noc.hpp"
#include "controller/interrupt/interrupt.hpp"
#include "controller/pcie/pcie.hpp"
#include "cache/cpu_port.hpp"
#include "../devices/hdd/hdd.hpp"
#include "../devices/nvme/nvme.hpp"
#include "../devices/nic/nic.hpp"

namespace colab {
struct SoCConfig {
    dimm::DIMMGeometry memory{4, {4, {2, {64, 128, 8}}}}; // 4 x 64 KiB = 256 KiB, 32-bit ranks.
    devices::hdd::Config disk{};
    interconnect::pcie::Config fabric{};
    soc::RV32Core::Mode cpu_mode = soc::RV32Core::Mode::Hardwired;
    soc::RV32Core::Execution cpu_execution = soc::RV32Core::Execution::Pipelined;
    soc::core::pipeline::Options pipeline{};
    bool enable_data_cache = true;
    soc::cache::Config data_cache = soc::cache::CpuPort::default_config();
};

// CPU data: MMU -> write-through Cache -> shared bus -> Memory -> DIMM.
// MMIO bypasses Cache; committed RAM writes snoop/update CPU cached copies.
// Instruction ROM remains Harvard-style, as in the existing RV32 core.
class SoC {
public:
    static constexpr std::uint32_t HddBase = 0x10000000, IrqBase = 0x10001000,
        NvmeBase = 0x20000000, NicBase = 0x30000000;
    static constexpr unsigned HddMaster = 1, NvmeMaster = 2, NicMaster = 3;
    explicit SoC(std::vector<std::uint32_t> program = {0x00000063}, SoCConfig config = {})
        : dimm_(config.memory), memory_(dimm_), bus_(iommu_, config.fabric),
          data_port_(bus_, config.data_cache, config.enable_data_cache), pcie_(bus_),
          disk_(config.disk), hdd_(disk_, bus_, HddMaster, irq_, 1),
          nvme_(bus_, NvmeMaster, irq_, 2), nic_(bus_, NicMaster, irq_, 3),
          cpu_(std::move(program), config.cpu_mode, 256, {}, config.cpu_execution, config.pipeline) {
        bus_.map_memory(0, config.memory.capacity_bytes(), memory_);
        pcie_.attach(HddBase, 0x100, hdd_);
        pcie_.attach(IrqBase, 0x100, irq_);
        pcie_.attach(NvmeBase, 0x2000, nvme_);
        pcie_.attach(NicBase, 0x100, nic_);
        memory_.set_write_observer([this](auto address, const auto& data) { data_port_.snoop_write(address, data); });
        cpu_.connect_bus(data_port_);
    }
    // Responses/side effects from queued requests, then CPU Cache and device/DMA state,
    // then interrupt sampling and one CPU clock. No requester ticks the fabric.
    // Newly submitted fabric requests wait until at least the next system tick.
    void tick() {
        bus_.tick();
        data_port_.tick();
        hdd_.tick(); nvme_.tick(); nic_.tick();
        cpu_.interrupts().set_external(irq_.requested());
        cpu_.tick();
        ++cycles_;
    }
    soc::RV32Core& cpu() { return cpu_; }
    soc::controller::Memory& memory() { return memory_; }
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
    soc::controller::IOMMU iommu_;
    Interconnect bus_;
    soc::cache::CpuPort data_port_;
    soc::controller::PCIe pcie_;
    soc::controller::Interrupt irq_;
    devices::HDD disk_;
    devices::hdd::IOController<devices::HDD> hdd_;
    devices::NVMe nvme_;
    devices::NIC nic_;
    soc::RV32Core cpu_;
    std::uint64_t cycles_ = 0;
};
}
