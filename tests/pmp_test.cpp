#include "../soc/rv32/pmp/pmp.hpp"
#include <iostream>
#include <random>

using Pmp = colab::soc::core::PhysicalMemoryProtection;
using Privilege = colab::soc::core::PrivilegeMode;
using Access = colab::soc::core::mmu::AccessType;
static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    Pmp reset;
    require(!reset.allows(0, 1, Access::Read, Privilege::User) && reset.allows(0, 1, Access::Read, Privilege::Machine),
        "PMP reset permissions differed");
    require(!reset.allows(0xffffffff, 2, Access::Read, Privilege::Machine) &&
        reset.allows(0xffffffff, 1, Access::Read, Privilege::Machine), "physical range overflow was not rejected");
    Pmp tor;
    tor.set_address(0, 0x100 / 4); tor.set_address(1, 0x200 / 4);
    tor.set_config(0, (Pmp::Tor | Pmp::R) | ((Pmp::Tor | Pmp::R | Pmp::W | Pmp::X) << 8));
    require(tor.allows(0, 4, Access::Read, Privilege::User) && !tor.allows(0, 4, Access::Write, Privilege::User) &&
        tor.allows(0x100, 4, Access::Write, Privilege::User), "TOR bounds or permissions differed");
    require(!tor.allows(0xfc, 8, Access::Read, Privilege::User) && !tor.allows(0xfc, 8, Access::Read, Privilege::Machine),
        "partial first match incorrectly fell through");
    Pmp priority;
    priority.set_address(0, 0x104 / 4); priority.set_address(1, (0x100 / 4) | 31);
    priority.set_config(0, Pmp::Na4 | ((Pmp::Napot | 7) << 8));
    require(!priority.allows(0x104, 4, Access::Read, Privilege::User) &&
        priority.allows(0x108, 4, Access::Write, Privilege::User) &&
        priority.allows(0x104, 4, Access::Read, Privilege::Machine), "NA4 priority or M bypass differed");
    priority.set_config(0, Pmp::Na4 | Pmp::Lock | ((Pmp::Napot | 7) << 8));
    priority.set_config(0, 0); priority.set_address(0, 0);
    require((priority.config(0) & 255) == (Pmp::Na4 | Pmp::Lock) && priority.address(0) == 0x104 / 4 &&
        !priority.allows(0x104, 4, Access::Read, Privilege::Machine), "locked entry was modified or bypassed");
    Pmp locked_tor;
    locked_tor.set_address(0, 0x100 / 4); locked_tor.set_address(1, 0x200 / 4);
    locked_tor.set_config(0, (Pmp::Tor | Pmp::Lock | Pmp::R) << 8);
    locked_tor.set_address(0, 0); locked_tor.set_address(1, 0);
    require(locked_tor.address(0) == 0x100 / 4 && locked_tor.address(1) == 0x200 / 4,
        "locked TOR did not freeze both bounds");
    Pmp groups;
    groups.set_address(15, 0xffffffff); groups.set_config(3, 0xff000000);
    require(groups.address(15) == 0x3fffffff && groups.config(3) == 0x9f000000, "RV32 PMP CSR packing/WARL failed");
    Pmp reserved;
    reserved.set_config(0, Pmp::Napot | Pmp::W | 0x60);
    require(reserved.config(0) == Pmp::Napot, "reserved PMP R/W configuration not canonicalized");
    std::mt19937 random(0x50504d);
    for (unsigned bits = 3; bits <= 32; ++bits) for (unsigned sample = 0; sample < 32; ++sample) {
        const auto size = std::uint64_t{1} << bits;
        const auto base = std::uint64_t(random()) / size * size;
        const auto encoded = std::uint32_t((base >> 2) | ((std::uint64_t{1} << (bits - 3)) - 1));
        for (bool lock : {false, true}) for (unsigned permission : {0u, 1u, 3u, 4u, 5u, 7u}) {
            Pmp pmp; pmp.set_address(0, encoded); pmp.set_config(0, Pmp::Napot | permission | (lock ? Pmp::Lock : 0));
            const auto address = std::uint32_t(base + (random() % size) / 4 * 4);
            const Access accesses[]{Access::Read, Access::Write, Access::Execute};
            for (unsigned access = 0; access < 3; ++access) {
                require(pmp.allows(address, 4, accesses[access], Privilege::User) == bool(permission & (1u << access)),
                    "NAPOT disagrees with independent region/permission reference");
                require(pmp.allows(address, 4, accesses[access], Privilege::Machine) == (!lock || bool(permission & (1u << access))),
                    "NAPOT M lock semantics differed");
            }
            if (base) require(!pmp.allows(std::uint32_t(base - 1), 1, Access::Read, Privilege::User), "NAPOT lower boundary leaked");
            if (base + size < (std::uint64_t{1} << 32))
                require(!pmp.allows(std::uint32_t(base + size), 1, Access::Read, Privilege::User), "NAPOT upper boundary leaked");
        }
    }
    std::cout << "PASS PMP: TOR/NA4/NAPOT, first-match/full-span rules, locks, WARL, RV32 boundaries and independent region reference\n";
}
