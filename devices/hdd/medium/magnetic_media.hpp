#pragma once

#include "../geometry.hpp"

#include <unordered_map>

namespace colab::devices::hdd {

// 磁层保存的是磁化方向，不是带电存储。为容纳题 44 的 40.96 GB 逻辑盘，
// 只给实际写过的扇区分配宿主内存；未分配扇区有均匀磁化方向，解码为全 0。
// NRZI 教学编码：数据 1 翻转极性，数据 0 保持极性；每扇区有独立同步基准。
class MagneticMedia {
public:
    explicit MagneticMedia(Geometry geometry) : geometry_(geometry) {}

    std::size_t allocated_sectors() const { return sectors_.size(); }
    bool inspect_polarity(Address address, std::size_t bit) const {
        if (bit >= geometry_.bytes_per_sector * 8) throw std::out_of_range("invalid magnetic cell");
        const auto found = sectors_.find(geometry_.encode(address));
        return found != sectors_.end() && ((found->second[bit / 8] >> (bit % 8)) & 1);
    }

private:
    friend class ReadWriteHead;
    Geometry geometry_;
    std::unordered_map<std::uint64_t, Bytes> sectors_;

    Bytes read(Address address, std::size_t offset, std::size_t count) const {
        Bytes result(count, 0);
        bool previous = offset ? inspect_polarity(address, offset * 8 - 1) : false;
        for (std::size_t i = 0; i < count * 8; ++i) {
            const bool polarity = inspect_polarity(address, offset * 8 + i);
            if (polarity != previous) result[i / 8] |= static_cast<std::uint8_t>(1u << (i % 8));
            previous = polarity;
        }
        return result;
    }
    void write(Address address, std::size_t offset, const Bytes& data) {
        if (data.empty()) return;
        const auto end = offset + data.size();
        const bool old_end = inspect_polarity(address, end * 8 - 1);
        bool polarity = offset ? inspect_polarity(address, offset * 8 - 1) : false;
        auto& cells = sectors_[geometry_.encode(address)];
        if (cells.empty()) cells.resize(static_cast<std::size_t>(geometry_.bytes_per_sector), 0);
        for (std::size_t i = 0; i < data.size() * 8; ++i) {
            if ((data[i / 8] >> (i % 8)) & 1) polarity = !polarity;
            const auto bit = offset * 8 + i;
            const auto mask = static_cast<std::uint8_t>(1u << (bit % 8));
            cells[bit / 8] = static_cast<std::uint8_t>((cells[bit / 8] & ~mask) | (polarity ? mask : 0));
        }
        // NRZI is differential: if the splice polarity changed, invert the
        // remaining cells together so their decoded bits remain unchanged.
        if (polarity != old_end)
            for (std::size_t i = end; i < cells.size(); ++i) cells[i] ^= 0xff;
    }
};

} // namespace colab::devices::hdd
