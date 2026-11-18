#pragma once

#include "../types.hpp"

#include <array>

namespace colab::soc::core::mmu {

class PageTableFormatError : public std::invalid_argument {
public:
    PageTableFormatError() : std::invalid_argument("reserved page table bits must be zero") {}
};

// 教学用的明确编码，不依赖 C++ sizeof，也不冒充某种现成 ISA 的 PTE 格式。
// [63:32] software_data， [31:12] PFN， [11:7] 保留， [6:0] D A U X W R P。
class PageTableEntryCodec {
public:
    using Bytes = std::array<std::uint8_t, 8>;
    static constexpr std::size_t byte_size = 8;

    static Bytes encode(const PageTableEntry& entry) {
        if (entry.physical_frame > max_page_number) throw std::out_of_range("physical frame exceeds 20 bits");
        const auto flags = std::uint64_t{entry.present}
            | (std::uint64_t{entry.permissions.readable} << 1)
            | (std::uint64_t{entry.permissions.writable} << 2)
            | (std::uint64_t{entry.permissions.executable} << 3)
            | (std::uint64_t{entry.permissions.user_accessible} << 4)
            | (std::uint64_t{entry.accessed} << 5) | (std::uint64_t{entry.dirty} << 6);
        const auto value = (std::uint64_t{entry.software_data} << 32)
            | (std::uint64_t{entry.physical_frame} << page_offset_bits) | flags;
        Bytes bytes{};
        for (std::size_t i = 0; i < byte_size; ++i) bytes[i] = static_cast<std::uint8_t>(value >> (8 * i));
        return bytes;
    }

    static PageTableEntry decode(const Bytes& bytes) {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < byte_size; ++i) value |= std::uint64_t{bytes[i]} << (8 * i);
        if ((value & 0xF80) != 0) throw PageTableFormatError();
        return {static_cast<Address>((value >> page_offset_bits) & max_page_number), (value & 1) != 0,
                {(value & 2) != 0, (value & 4) != 0, (value & 8) != 0, (value & 16) != 0},
                (value & 32) != 0, (value & 64) != 0, static_cast<std::uint32_t>(value >> 32)};
    }
};

} // namespace colab::soc::core::mmu
