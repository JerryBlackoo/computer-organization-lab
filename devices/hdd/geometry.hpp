#pragma once

#include "types.hpp"

#include <limits>

namespace colab::devices::hdd {

namespace detail {
inline std::uint64_t multiply(std::uint64_t a, std::uint64_t b) {
    if (b && a > std::numeric_limits<std::uint64_t>::max() / b)
        throw std::overflow_error("HDD geometry overflow");
    return a * b;
}
inline unsigned address_bits(std::uint64_t count) {
    unsigned bits = 0;
    for (auto top = count - 1; top; top >>= 1) ++bits;
    return bits;
}
} // namespace detail

struct Dimensions {
    Time inner_radius_mm = 20;
    Time outer_radius_mm = 50;
};

struct AddressWidths { unsigned cylinder, head, sector; };

struct Density {
    Time tracks_per_mm;
    Time payload_bits_per_mm;
    Time payload_bits_per_mm2;
    std::optional<Time> raw_bits_per_mm;
    std::optional<Time> raw_bits_per_mm2;
};

struct TrackLocation {
    std::uint64_t platter;
    unsigned side;
    std::uint64_t cylinder;
    std::uint64_t sectors;
};

struct Geometry {
    // 第 44 题：4 个双面盘片、8 个记录面/磁头，而不是 8 个柱面。
    std::uint64_t platters = 4;
    std::uint64_t cylinders = 20000;
    std::uint64_t sectors_per_track = 500;
    std::uint64_t bytes_per_sector = 512;
    // 未给磁化单元/格式开销时不编造非格式化容量。给定后，多出的槽位
    // 均摊到各扇区开头，代表地址、同步、校验和间隙，不作为用户字节。
    std::optional<std::uint64_t> raw_bytes_per_track;
    std::optional<Dimensions> dimensions;

    std::uint64_t heads() const { return detail::multiply(platters, 2); }
    std::uint64_t tracks() const { return detail::multiply(heads(), cylinders); }
    std::uint64_t sectors() const { return detail::multiply(tracks(), sectors_per_track); }
    std::uint64_t capacity_bytes() const { return detail::multiply(sectors(), bytes_per_sector); }
    std::uint64_t payload_bytes_per_track() const {
        return detail::multiply(sectors_per_track, bytes_per_sector);
    }
    std::optional<std::uint64_t> unformatted_capacity_bytes() const {
        if (!raw_bytes_per_track) return std::nullopt;
        return detail::multiply(tracks(), *raw_bytes_per_track);
    }

    void validate() const {
        if (!platters || !cylinders || !sectors_per_track || !bytes_per_sector ||
            bytes_per_sector > std::numeric_limits<std::size_t>::max() / 8)
            throw std::invalid_argument("invalid HDD geometry");
        (void)capacity_bytes();
        if (raw_bytes_per_track && *raw_bytes_per_track < payload_bytes_per_track())
            throw std::invalid_argument("raw track is smaller than its payload");
        (void)unformatted_capacity_bytes();
        if (dimensions && (!std::isfinite(dimensions->inner_radius_mm) ||
            !std::isfinite(dimensions->outer_radius_mm) || dimensions->inner_radius_mm <= 0 ||
            dimensions->outer_radius_mm <= dimensions->inner_radius_mm))
            throw std::invalid_argument("invalid HDD radii");
    }

    void check(Address address) const {
        if (address.cylinder >= cylinders || address.head >= heads() ||
            address.sector >= sectors_per_track) throw std::out_of_range("invalid HDD CHS");
    }
    AddressWidths address_widths() const {
        validate();
        return {detail::address_bits(cylinders), detail::address_bits(heads()),
                detail::address_bits(sectors_per_track)};
    }
    std::uint64_t encode(Address address) const {
        check(address);
        return (address.cylinder * heads() + address.head) * sectors_per_track + address.sector;
    }
    Address decode(std::uint64_t lba) const {
        if (lba >= sectors()) throw std::out_of_range("invalid HDD LBA");
        return {lba / sectors_per_track / heads(), lba / sectors_per_track % heads(),
                lba % sectors_per_track};
    }
    TrackLocation track(std::uint64_t cylinder, std::uint64_t head) const {
        check({cylinder, head, 0});
        return {head / 2, static_cast<unsigned>(head % 2), cylinder, sectors_per_track};
    }
    Density density(std::uint64_t cylinder) const {
        if (!dimensions) throw std::logic_error("disk radii were not specified");
        check({cylinder, 0, 0});
        const auto pitch = (dimensions->outer_radius_mm - dimensions->inner_radius_mm) / cylinders;
        const auto radius = dimensions->outer_radius_mm - (cylinder + 0.5L) * pitch;
        const auto circumference = 2 * std::acos(-1.0L) * radius;
        const auto linear = static_cast<Time>(payload_bytes_per_track()) * 8 / circumference;
        Density result{1 / pitch, linear, linear / pitch, {}, {}};
        if (raw_bytes_per_track) {
            result.raw_bits_per_mm = static_cast<Time>(*raw_bytes_per_track) * 8 / circumference;
            result.raw_bits_per_mm2 = *result.raw_bits_per_mm / pitch;
        }
        return result;
    }
};

struct Config {
    Geometry geometry{};
    Time rpm = 7200;
    Time average_seek_ms = 5;
    Time spin_up_ms = 1000; // 以下电机/控制器参数是可改的教学假设，不来自第 44 题。
    Time coast_down_ms = 500;
    Time controller_ms = 0;
    Time dma_bus_cycle_ms = 0.00005L; // 一个 64 bit 主存总线周期 50 ns。
    std::size_t dma_buffer_bits = 64;
    Time tick_ms = 0.001L;

    void validate() const {
        geometry.validate();
        for (const auto value : {rpm, average_seek_ms, spin_up_ms, coast_down_ms,
                                 controller_ms, dma_bus_cycle_ms, tick_ms}) check_time(value);
        if (!rpm || !spin_up_ms || !coast_down_ms || !dma_bus_cycle_ms || !tick_ms ||
            !dma_buffer_bits || dma_buffer_bits % 8 ||
            dma_buffer_bits / 8 > geometry.bytes_per_sector)
            throw std::invalid_argument("invalid HDD drive/DMA configuration");
        if (!std::isfinite(60000 / rpm) || !std::isfinite(2 * average_seek_ms) ||
            sector_ms() <= 0 || sector_ms() * payload_fraction() / geometry.bytes_per_sector <= 0)
            throw std::invalid_argument("HDD timing overflow/underflow");
    }
    Time revolution_ms() const { return 60000 / rpm; }
    Time sector_ms() const { return revolution_ms() / geometry.sectors_per_track; }
    Time average_rotation_ms() const { return revolution_ms() / 2; }
    Time average_access_ms() const { return average_seek_ms + average_rotation_ms() + sector_ms(); }
    Time payload_bytes_per_second() const { return geometry.payload_bytes_per_track() * rpm / 60; }
    std::uint64_t dma_requests_per_sector() const {
        const auto bytes = dma_buffer_bits / 8;
        return geometry.bytes_per_sector / bytes + (geometry.bytes_per_sector % bytes != 0);
    }
    Time payload_fraction() const {
        return geometry.raw_bytes_per_track ?
            static_cast<Time>(geometry.payload_bytes_per_track()) / *geometry.raw_bytes_per_track : 1;
    }
};

} // namespace colab::devices::hdd
