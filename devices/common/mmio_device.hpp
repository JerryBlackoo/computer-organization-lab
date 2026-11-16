#pragma once

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace colab::devices::common {
using Bytes = std::vector<std::uint8_t>;

inline std::uint32_t load32(const Bytes& bytes, std::size_t offset = 0) {
    if (offset > bytes.size() || bytes.size() - offset < 4) throw std::out_of_range("short little-endian word");
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= std::uint32_t(bytes[offset + i]) << (8 * i);
    return value;
}
inline void store32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    if (offset > bytes.size() || bytes.size() - offset < 4) throw std::out_of_range("short little-endian word");
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
}
inline Bytes word_bytes(std::uint32_t value) { Bytes result(4); store32(result, 0, value); return result; }
inline Bytes scalar_bytes(std::uint32_t value, unsigned count) {
    if (count != 1 && count != 2 && count != 4) throw std::invalid_argument("invalid scalar width");
    Bytes result(count);
    for (unsigned i = 0; i < count; ++i) result[i] = static_cast<std::uint8_t>(value >> (8 * i));
    return result;
}
inline std::uint32_t load_scalar(const Bytes& bytes) {
    if (bytes.size() != 1 && bytes.size() != 2 && bytes.size() != 4)
        throw std::invalid_argument("invalid scalar response width");
    std::uint32_t value = 0;
    for (unsigned i = 0; i < bytes.size(); ++i) value |= std::uint32_t(bytes[i]) << (8 * i);
    return value;
}
inline std::uint64_t load64(const Bytes& bytes, std::size_t offset) {
    return load32(bytes, offset) | (std::uint64_t(load32(bytes, offset + 4)) << 32);
}

class MmioDevice {
public:
    virtual ~MmioDevice() = default;
    virtual std::uint32_t mmio_read32(std::uint32_t offset) = 0;
    virtual void mmio_write32(std::uint32_t offset, std::uint32_t value) = 0;
    virtual void tick() = 0;
};
} // namespace colab::devices::common
