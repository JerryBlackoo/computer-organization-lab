#include "../devices/clint/clint.hpp"
#include <iostream>

using Clint = colab::devices::Clint;
static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static void time(Clint& clint, std::uint64_t value) {
    clint.mmio_write32(Clint::MtimeHigh, std::uint32_t(value >> 32));
    clint.mmio_write32(Clint::MtimeLow, std::uint32_t(value));
}
static void compare(Clint& clint, std::uint64_t value) {
    clint.mmio_write32(Clint::MtimecmpLow, 0xffffffff);
    clint.mmio_write32(Clint::MtimecmpHigh, std::uint32_t(value >> 32));
    clint.mmio_write32(Clint::MtimecmpLow, std::uint32_t(value));
}
int main() {
    Clint clint;
    require(clint.time() == 0 && clint.compare() == UINT64_MAX && !clint.timer_pending() && !clint.software_pending(),
        "CLINT reset values differed");
    compare(clint, 3);
    clint.tick(); clint.tick(); require(!clint.timer_pending(), "timer fired before unsigned compare");
    clint.tick(); require(clint.timer_pending(), "timer equality did not assert MTIP");
    clint.tick(); require(clint.timer_pending(), "timer level did not remain pending");
    compare(clint, 9); require(!clint.timer_pending(), "future compare did not clear MTIP");
    clint.mmio_write32(Clint::Msip, 0xfffffffe); require(clint.mmio_read32(Clint::Msip) == 0, "MSIP reserved bits affected level");
    clint.mmio_write32(Clint::Msip, 0xffffffff); require(clint.mmio_read32(Clint::Msip) == 1 && clint.software_pending(), "MSIP level failed");
    clint.mmio_write32(Clint::Msip, 0); require(!clint.software_pending(), "MSIP acknowledgement failed");
    time(clint, 0x1fffffff0); compare(clint, 0x1ffffffff);
    // The RV32 low=-1, high, low sequence must not transiently lower the compare.
    clint.mmio_write32(Clint::MtimecmpLow, 0xffffffff); require(!clint.timer_pending(), "safe low-half sentinel raised MTIP");
    clint.mmio_write32(Clint::MtimecmpHigh, 2); require(!clint.timer_pending(), "safe high-half update raised MTIP");
    clint.mmio_write32(Clint::MtimecmpLow, 0x10); require(!clint.timer_pending(), "future 64-bit compare raised MTIP");
    compare(clint, 0x1ffffffff); clint.mmio_write32(Clint::MtimecmpLow, 0x10);
    require(clint.timer_pending(), "unsafe RV32 split write was incorrectly treated as atomic 64-bit write");
    time(clint, UINT64_MAX); compare(clint, UINT64_MAX); require(clint.timer_pending(), "64-bit equality compare failed");
    clint.tick(); require(clint.time() == 0 && !clint.timer_pending(), "mtime did not wrap modulo 2^64");
    time(clint, 0x1ffffffff);
    const auto high_before = clint.mmio_read32(Clint::MtimeHigh);
    clint.tick();
    require(high_before == 1 && clint.mmio_read32(Clint::MtimeLow) == 0 && clint.mmio_read32(Clint::MtimeHigh) == 2,
        "RV32 mtime halves did not expose rollover for high/low/high retry");
    const auto saved_time = clint.time(), saved_compare = clint.compare();
    for (auto offset : {1u, 4u, 0x4008u, 0xbff4u}) {
        bool read = false, write = false;
        try { (void)clint.mmio_read32(offset); } catch (const std::out_of_range&) { read = true; }
        try { clint.mmio_write32(offset, 0); } catch (const std::out_of_range&) { write = true; }
        require(read && write && clint.time() == saved_time && clint.compare() == saved_compare,
            "invalid CLINT register altered state");
    }
    std::cout << "PASS CLINT: 64-bit time/compare, level interrupts, software IRQ, RV32 split writes, rollover and register bounds\n";
}
