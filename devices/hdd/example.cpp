#include "hdd.hpp"

#include <iomanip>
#include <iostream>
#include <stdexcept>

using colab::devices::HDD;
using namespace colab::devices::hdd;

Completion wait_for_sector(HDD& disk) {
    if (!disk.run_until_idle(100)) throw std::runtime_error("HDD did not finish in 100 ms");
    auto result = disk.take_completion();
    if (!result || result->status != Status::Success) throw std::runtime_error("HDD command failed");
    return std::move(*result);
}

void print_measurement(const char* name, const Completion& result) {
    const auto& m = result.measurement;
    std::cout << name << ": seek=" << m.seek_ms << " ms, rotation=" << m.rotation_ms
              << " ms, transfer=" << m.transfer_ms << " ms, access=" << m.access_ms()
              << " ms, response=" << m.response_ms << " ms\n"
              << "  DMA requests=" << m.dma_requests << ", grants=" << m.dma_grants
              << ", CPU contentions=" << m.dma_cpu_contentions << ", bytes=" << m.dma_bytes << '\n';
}

int main() {
    HDD disk;
    const auto& config = disk.config();
    const auto widths = disk.geometry().address_widths();
    std::cout << std::fixed << std::setprecision(6)
              << "Exercise 44: " << disk.geometry().platters << " platters, "
              << disk.geometry().heads() << " heads, " << disk.geometry().cylinders
              << " cylinders, " << disk.geometry().tracks() << " tracks\n"
              << "CHS widths: " << widths.cylinder << '/' << widths.head << '/' << widths.sector
              << " bits\nFormatted capacity: " << disk.geometry().capacity_bytes() << " B\n"
              << "Revolution: " << config.revolution_ms() << " ms, average rotation: "
              << config.average_rotation_ms() << " ms\nSector transfer: " << config.sector_ms()
              << " ms, payload rate: " << config.payload_bytes_per_second() << " B/s\n"
              << "Reference average access: " << config.average_access_ms() << " ms\n\n"
              << "Before power: rpm=" << disk.spindle().rpm() << ", ready=" << disk.ready() << '\n';

    disk.power_on();
    disk.advance(500);
    std::cout << "After 500 ms powered: rpm=" << disk.spindle().rpm() << ", ready=" << disk.ready() << '\n';
    disk.advance(500);
    std::cout << "After 1000 ms powered: rpm=" << disk.spindle().rpm() << ", ready=" << disk.ready() << '\n';
    disk.set_bus_signals({true, false}); // CPU 同时申请时，DMA 优先获得空闲的总线周期。
    Bytes payload(512);
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::uint8_t>(i * 73 + 5);
    const Address address{10000, 6, 123};
    disk.submit_write(address, payload);
    print_measurement("Write", wait_for_sector(disk));
    disk.power_off();
    disk.advance(config.coast_down_ms);
    std::cout << "After power loss and coast: rpm=" << disk.spindle().rpm()
              << ", magnetic sectors=" << disk.media().allocated_sectors() << '\n';
    disk.power_on();
    disk.advance(config.spin_up_ms);
    disk.submit_read(address);
    const auto read = wait_for_sector(disk);
    print_measurement("Read after restart", read);
    if (read.data != payload) throw std::runtime_error("nonvolatile roundtrip mismatch");
    std::cout << "Magnetic data survived power cycle: true\n\n";

    // 用实际的寻道、角位置、传输事件采样均值，不直接输出 average_access_ms()。
    // 此受控样本一半零距离、一半全程寻道；不宣称随机工作负载也恰好平均 5 ms。
    Time measured_seek = 0, measured_rotation = 0, measured_transfer = 0;
    constexpr unsigned samples = 64;
    for (unsigned i = 0; i < samples; ++i) {
        HDD probe;
        probe.power_on();
        probe.advance(config.spin_up_ms);
        const Time seek = i % 2 ? 10 : 0;
        auto initial_phase = std::fmod((i + 0.5L) / samples - seek / config.revolution_ms(), 1.0L);
        if (initial_phase < 0) initial_phase += 1;
        probe.advance(initial_phase * config.revolution_ms());
        probe.submit_read({i % 2 ? 19999ULL : 0ULL, 0, 0});
        const auto m = wait_for_sector(probe).measurement;
        measured_seek += m.seek_ms;
        measured_rotation += m.rotation_ms;
        measured_transfer += m.transfer_ms;
    }
    std::cout << "Measured " << samples << "-sample average: seek=" << measured_seek / samples
              << " ms, rotation=" << measured_rotation / samples
              << " ms, transfer=" << measured_transfer / samples
              << " ms, access=" << (measured_seek + measured_rotation + measured_transfer) / samples << " ms\n";
}
