#include "io_test_support.hpp"
using namespace io_test;

void pio_write_and_recover() {
    SoC system({0x00000063}, config()); seed_hdd(system);
    // Invalid programming produces a visible error, then a valid command recovers.
    write(system, SoC::HddBase + Block::Length, 0);
    write(system, SoC::HddBase + Block::Command, Block::Read);
    assert(read(system, SoC::HddBase + Block::Status) == Block::Failed);
    write(system, SoC::HddBase + Block::Length, 512);
    write(system, SoC::HddBase + Block::Command, Block::Write);
    const auto data = pattern();
    for (std::size_t offset = 0; offset < data.size(); offset += 4) {
        until(system, [&] { return system.hdd().state() == devices::hdd::DeviceState::DataReady; });
        write(system, SoC::HddBase + Block::Data, c::load32(data, offset));
    }
    until(system, [&] { return system.hdd().state() == devices::hdd::DeviceState::Completed; });
    write(system, SoC::HddBase + Block::DmaAddress, 0x800);
    write(system, SoC::HddBase + Block::Control, Block::EnableDma);
    write(system, SoC::HddBase + Block::Command, Block::Read);
    until(system, [&] { return system.hdd().state() == devices::hdd::DeviceState::Completed; });
    assert(system.memory().read(0x800, 512) == data);
    // Device DMA write is read back from the actual magnetic medium.
    system.memory().write(0x800, c::Bytes(512, 0xA5));
    write(system, SoC::HddBase + Block::Command, Block::Write);
    until(system, [&] { return system.hdd().state() == devices::hdd::DeviceState::Completed; });
    system.memory().write(0x800, c::Bytes(512));
    write(system, SoC::HddBase + Block::Command, Block::Read);
    until(system, [&] { return system.hdd().state() == devices::hdd::DeviceState::Completed; });
    assert(system.memory().read(0x800, 512) == c::Bytes(512, 0xA5));
    write(system, SoC::HddBase + Block::Command, Block::Read);
    system.disk().power_off();
    until(system, [&] { return system.hdd().state() == devices::hdd::DeviceState::Error; });
}

void nvme_permissions_and_prps() {
    SoC system({0x00000063}, config()); create_io_queue(system); system.nvme().nand().write(0, pattern());
    system.iommu().map({SoC::NvmeMaster, 0x1000, 0x1000, 0x4000, true, true});
    system.iommu().map({SoC::NvmeMaster, 0x5000, 0x5000, 0x1000, true, false}); system.iommu().enable();
    system.memory().write(0x3000, command(2, 10, 0x5000, 1));
    write(system, SoC::NvmeBase + 0x1008, 1);
    until(system, [&] { return system.nvme().controller().completed_commands() == 3; });
    assert(system.memory().read(0x5000, 512) == c::Bytes(512));
    assert((c::load32(system.memory().read(0x400c, 4)) >> 17) == 4); // Data Transfer Error, not false success.
    write(system, SoC::NvmeBase + 0x100c, 1); system.iommu().enable(false);
    // Two physical PRPs, not an assumption that all buffers are contiguous.
    auto cmd = command(2, 11, 0x5F00, 1);
    c::store32(cmd, 32, 0x7000); system.memory().write(0x3040, cmd);
    write(system, SoC::NvmeBase + 0x1008, 2);
    until(system, [&] { return system.nvme().controller().completed_commands() == 4; });
    const auto expected = pattern();
    assert(system.memory().read(0x5F00, 256) == c::Bytes(expected.begin(), expected.begin() + 256));
    assert(system.memory().read(0x7000, 256) == c::Bytes(expected.begin() + 256, expected.end()));
    assert((c::load32(system.memory().read(0x401c, 4)) >> 17) == 0);
    write(system, SoC::NvmeBase + 0x100c, 2);
    c::store32(cmd, 32, 0x7004); system.memory().write(0x3080, cmd);
    write(system, SoC::NvmeBase + 0x1008, 3);
    until(system, [&] { return system.nvme().controller().completed_commands() == 5; });
    assert((c::load32(system.memory().read(0x402c, 4)) >> 17) == 2);
    write(system, SoC::NvmeBase + 0x100c, 3);
    // Revoke all permissions for a new bus master: SQ fetch failure is fatal, no fabricated CQE.
    SoC denied({0x00000063}, config()); create_io_queue(denied); denied.iommu().enable();
    denied.memory().write(0x3000, command(2, 10, 0x5000, 1));
    write(denied, SoC::NvmeBase + 0x1008, 1);
    until(denied, [&] { return denied.nvme().controller().state() == devices::nvme::Controller::State::Fatal; });
    assert(denied.nvme().controller().completed_commands() == 2 && !denied.interrupts().requested());
    assert(denied.bus().idle());
    SoC cq_denied({0x00000063}, config()); create_io_queue(cq_denied);
    cq_denied.nvme().nand().write(0, pattern());
    cq_denied.iommu().map({SoC::NvmeMaster, 0x3000, 0x3000, 0x1000, true, false});
    cq_denied.iommu().map({SoC::NvmeMaster, 0x5000, 0x5000, 0x1000, true, true});
    cq_denied.iommu().enable();
    cq_denied.memory().write(0x3000, command(2, 10, 0x5000, 1));
    write(cq_denied, SoC::NvmeBase + 0x1008, 1);
    until(cq_denied, [&] { return cq_denied.nvme().controller().state() == devices::nvme::Controller::State::Fatal; });
    assert(cq_denied.memory().read(0x5000, 512) == pattern());
    assert(cq_denied.memory().read(0x4000, 16) == c::Bytes(16));
    assert(cq_denied.nvme().controller().completed_commands() == 2 && !cq_denied.interrupts().requested());
}

void cq_backpressure() {
    SoC system({0x00000063}, config()); create_io_queue(system);
    for (unsigned slot = 0; slot < 3; ++slot) system.memory().write(0x3000 + slot * 64, command(2, slot, 0x5000, 1));
    write(system, SoC::NvmeBase + 0x1008, 3);
    until(system, [&] { return system.nvme().controller().completed_commands() == 5; });
    const auto old = system.memory().read(0x4000, 48);
    system.memory().write(0x30c0, command(2, 3, 0x5000, 1));
    write(system, SoC::NvmeBase + 0x1008, 0);
    for (unsigned cycle = 0; cycle < 500; ++cycle) system.tick();
    assert(system.nvme().controller().completed_commands() == 5 && system.memory().read(0x4000, 48) == old);
    write(system, SoC::NvmeBase + 0x100c, 1);
    until(system, [&] { return system.nvme().controller().completed_commands() == 6; });
    assert(c::load32(system.memory().read(0x403c, 4)) == 0x10003);
    // Reprogramming active queue bases and out-of-range doorbells return bus errors.
    assert(transfer(system, SoC::NvmeBase + devices::nvme::Controller::AdminSq, true, 0x8000).status ==
        interconnect::pcie::Status::BackendError);
    assert(transfer(system, SoC::NvmeBase + 0x1008, true, 4).status == interconnect::pcie::Status::BackendError);
}

void cpu_interrupt_boundary_and_faults() {
    for (const auto& cpu_config : cpu_configs()) {
        std::vector<std::uint32_t> program(32, 0x00000063);
        program[0] = 0x06002403; // LW x8, 0x60(x0).
        program[27] = 0x30200073; // Vectored mtvec BASE=64, external cause=11 -> 108.
        SoC system(program, cpu_config); system.memory().write(0x60, c::word_bytes(0x76543210));
        system.cpu().interrupts().write(0x305, 0x41);
        system.cpu().interrupts().write(0x304, 1u << 11); system.cpu().interrupts().write(0x300, 8);
        until(system, [&] {
            if (system.cpu().execution() == soc::RV32Core::Execution::Pipelined)
                return bool(system.cpu().pipeline().ex_mem().request);
            return system.cpu().control_unit().rv32_phase() == soc::core::control_unit::rv32::Phase::LoadRead;
        });
        system.interrupts().set_irq(1, true);
        while (system.cpu().completed_instructions() == 0) {
            system.tick(); assert(system.cpu().interrupts().entries() == 0);
        }
        system.tick();
        assert(system.cpu().pc() == 108 && system.cpu().interrupts().read(0x341) == 4);
        assert(system.cpu().last_cycle()->interrupt_entry);
        assert(system.cpu().registers().read(8) == 0x76543210);
        assert(!(system.cpu().interrupts().read(0x300) & 8));
        system.interrupts().set_irq(1, false);
        until(system, [&] { return system.cpu().interrupts().returns() == 1; });
        assert(system.cpu().pc() == 4 && (system.cpu().interrupts().read(0x300) & 8));
    }
    SoC fault({0x0000A183}, config()); // LW x3, 0(x1).
    fault.cpu().set_register(1, 0x40000000); fault.cpu().set_register(3, 0x1234);
    bool trapped = false;
    try { for (unsigned n = 0; n < 100; ++n) fault.tick(); }
    catch (const soc::core::RV32Fault& error) {
        trapped = true; assert(error.cause() == soc::core::RV32FaultCause::LoadAccessFault);
        assert(error.pc() == 0 && error.value() == 0x40000000);
    }
    assert(trapped && fault.cpu().completed_instructions() == 0 && fault.cpu().registers().read(3) == 0x1234);
}

void bus_backpressure() {
    auto cfg = config(); cfg.fabric.max_outstanding = 1;
    SoC system({0x00000063}, cfg);
    const auto occupied = system.bus().submit(0, c::BusOperation::Read, 0, 4);
    c::DmaEngine dma(system.bus(), 7);
    dma.start({0x800, 4, c::DmaDirection::DeviceToMemory}, {1, 2, 3, 4}); dma.tick();
    assert(dma.busy() && dma.transferred() == 0);
    for (unsigned n = 0; n < 100 && dma.busy(); ++n) { system.tick(); dma.tick(); }
    assert(dma.completed() && system.bus().take(occupied).data == c::Bytes(4));
    assert(system.memory().read(0x800, 4) == c::Bytes({1, 2, 3, 4}));
    throws<std::invalid_argument>([&] { system.bus().submit(0, c::BusOperation::Read, SoC::HddBase, 8); });
    throws<std::invalid_argument>([&] { system.bus().submit(0, c::BusOperation::Read, SoC::HddBase + 1, 4); });
}

int main() {
    pio_write_and_recover(); nvme_permissions_and_prps(); cq_backpressure();
    cpu_interrupt_boundary_and_faults(); bus_backpressure();
    std::cout << "PASS I/O edge cases: errors/recovery, PRPs, CQ pressure, precise IRQ/faults, bus backpressure\n";
}
