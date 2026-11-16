#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace colab::simulation::boot {
struct Segment {
    std::uint32_t address = 0, memory_size = 0, flags = 0;
    std::vector<std::uint8_t> data;
};
struct ElfImage {
    std::uint32_t entry = 0;
    std::vector<Segment> segments;
};

// ELF32 little-endian, fixed-address, static RISC-V executables only.
inline ElfImage parse_elf32(const std::vector<std::uint8_t>& bytes, std::uint64_t ram_size) {
    const auto range = [&](std::uint64_t offset, std::uint64_t size) {
        return offset <= bytes.size() && size <= bytes.size() - offset;
    };
    const auto word = [&](std::uint64_t offset, unsigned size) {
        if (!range(offset, size)) throw std::invalid_argument("truncated ELF field");
        std::uint32_t value = 0;
        for (unsigned i = 0; i < size; ++i) value |= std::uint32_t(bytes[offset + i]) << (8 * i);
        return value;
    };
    if (!range(0, 52) || bytes[0] != 0x7f || bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F' ||
        bytes[4] != 1 || bytes[5] != 1 || bytes[6] != 1)
        throw std::invalid_argument("expected a complete little-endian ELF32 header");
    if (word(16, 2) != 2 || word(18, 2) != 243 || word(20, 4) != 1 || word(40, 2) != 52)
        throw std::invalid_argument("expected a static RISC-V ELF32 executable");
    if (word(36, 4) != 0) throw std::invalid_argument("unsupported RISC-V ELF ABI flags");
    const auto phoff = word(28, 4), phentsize = word(42, 2), phnum = word(44, 2);
    if (!phnum || phnum == 0xffff || phoff < 52 || phentsize != 32 || !range(phoff, std::uint64_t(phentsize) * phnum))
        throw std::invalid_argument("invalid ELF program header table");
    ElfImage image; image.entry = word(24, 4);
    bool executable_entry = false;
    for (std::uint32_t i = 0; i < phnum; ++i) {
        const auto p = std::uint64_t(phoff) + std::uint64_t(i) * phentsize;
        const auto type = word(p, 4);
        if (type == 2 || type == 3) throw std::invalid_argument("dynamic ELF executables are unsupported");
        if (type != 1) continue;
        const auto offset = word(p + 4, 4), vaddr = word(p + 8, 4), paddr = word(p + 12, 4);
        const auto file_size = word(p + 16, 4), mem_size = word(p + 20, 4);
        const auto flags = word(p + 24, 4), align = word(p + 28, 4);
        if (vaddr != paddr || file_size > mem_size || !range(offset, file_size) ||
            std::uint64_t(vaddr) + mem_size > ram_size || std::uint64_t(vaddr) + mem_size > (std::uint64_t{1} << 32) ||
            (align > 1 && ((align & (align - 1)) || (vaddr % align != offset % align))))
            throw std::invalid_argument("invalid ELF load segment");
        if (!mem_size) continue;
        for (const auto& previous : image.segments)
            if (std::uint64_t(vaddr) < std::uint64_t(previous.address) + previous.memory_size &&
                std::uint64_t(previous.address) < std::uint64_t(vaddr) + mem_size)
                throw std::invalid_argument("overlapping ELF load segments");
        if ((flags & 1) && image.entry >= vaddr && std::uint64_t(image.entry) + 4 <= std::uint64_t(vaddr) + file_size)
            executable_entry = true;
        Segment segment{vaddr, mem_size, flags, {}};
        segment.data.assign(bytes.begin() + offset, bytes.begin() + offset + file_size);
        image.segments.push_back(std::move(segment));
    }
    if (image.entry % 4 || !executable_entry || image.segments.empty())
        throw std::invalid_argument("ELF entry is not an aligned executable instruction");
    return image;
}
} // namespace colab::simulation::boot
