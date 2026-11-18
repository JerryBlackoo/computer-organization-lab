#pragma once

#include <cstdint>

namespace colab::soc::address_map {

struct Window {
    std::uint32_t base;
    std::uint64_t size;
};

// Default board MMIO windows shared by the SoC and SystemC models.
inline constexpr Window Hdd{0x10000000, 0x100};
inline constexpr Window Irq{0x10001000, 0x100};
inline constexpr Window Nvme{0x20000000, 0x2000};
inline constexpr Window Nic{0x30000000, 0x100};

// Additional SystemC platform windows.
inline constexpr Window Clint{0x02000000, 0xc000};
inline constexpr Window Timer{0x40000000, 0x100};
inline constexpr Window Uart{0x40001000, 0x100};

} // namespace colab::soc::address_map
