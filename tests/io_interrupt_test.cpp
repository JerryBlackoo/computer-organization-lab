#include "io_test_support.hpp"
using namespace io_test;

int main() {
    for (const auto& cpu_config : cpu_configs()) {
        const auto image = examples::guest::hdd_read(Mode::Interrupt);
        SoC system(image.instructions, cpu_config); image.load_constants(system.memory()); seed_hdd(system);
        system.cpu().set_register(20, 0xABCDEF01); system.cpu().set_register(22, 0x12345678);
        run_guest(system);
        assert(system.memory().read(0x800, 512) == pattern());
        assert(system.cpu().interrupts().entries() == 128);
        assert(system.cpu().interrupts().returns() == 128);
        assert(system.cpu().registers().read(20) == 0xABCDEF01);
        assert(system.cpu().registers().read(22) == 0x12345678);
        assert(system.cpu().registers().read(2) == 0x400);
        assert(system.cpu().registers().read(9) > 0);
        assert(!system.interrupts().requested());
        assert(system.cpu().interrupts().read(0x342) == 0x8000000B);
    }
    soc::controller::Interrupt irq;
    irq.set_irq(3, true); irq.set_irq(1, true);
    irq.mmio_write32(irq.Mask, 1u << 1);
    assert(irq.mmio_read32(irq.Claim) == 3);
    irq.mmio_write32(irq.Complete, 3); // An unacknowledged device level is re-pended.
    assert(irq.requested());
    irq.set_irq(3, false); assert(!irq.requested());
    irq.mmio_write32(irq.Mask, 0); assert(irq.mmio_read32(irq.Claim) == 1);
    irq.set_irq(1, false); irq.mmio_write32(irq.Complete, 1); assert(!irq.requested());
    std::cout << "PASS IRQ: guest ISR/MRET, PC and GPR restore, mask/claim/complete\n";
}
