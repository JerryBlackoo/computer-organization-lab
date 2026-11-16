#include "../devices/hdd/hdd.hpp"
#include "../simulation/simulator.hpp"

#include <cassert>
#include <iostream>
#include <limits>

using colab::devices::HDD;
using namespace colab::devices::hdd;

template <typename Exception, typename Function>
void expect_throw(Function function) {
    bool caught = false;
    try { function(); } catch (const Exception&) { caught = true; }
    assert(caught);
}

void near(Time actual, Time expected, Time tolerance = 1e-9L) {
    if (std::abs(actual - expected) > tolerance) {
        std::cerr << "actual=" << static_cast<double>(actual)
                  << " expected=" << static_cast<double>(expected) << '\n';
        assert(false);
    }
}

Completion completed(HDD& disk) {
    assert(disk.run_until_idle(2000));
    auto result = disk.take_completion();
    assert(result);
    return std::move(*result);
}

void ready(HDD& disk) {
    disk.power_on();
    disk.advance(disk.config().spin_up_ms);
    assert(disk.ready());
}

void check_accounting(const Measurement& m) {
    near(m.response_ms, m.queue_ms + m.controller_ms + m.prefetch_ms + m.seek_ms +
         m.rotation_ms + m.transfer_ms + m.drain_ms);
}

void test_geometry_and_reference_metrics() {
    Geometry g;
    assert(g.heads() == 8);
    assert(g.cylinders == 20000);
    assert(g.tracks() == 160000);
    assert(g.sectors() == 80000000);
    assert(g.capacity_bytes() == 40960000000ULL);
    const auto widths = g.address_widths();
    assert(widths.cylinder == 15 && widths.head == 3 && widths.sector == 9);
    const auto last = g.decode(g.sectors() - 1);
    assert(last.cylinder == 19999 && last.head == 7 && last.sector == 499);
    assert(g.encode(last) == g.sectors() - 1);
    const auto track = g.track(19999, 7);
    assert(track.platter == 3 && track.side == 1 && track.cylinder == 19999 && track.sectors == 500);
    for (std::uint64_t i = 0; i < 2000; ++i) {
        const auto lba = i * 39999;
        assert(g.encode(g.decode(lba)) == lba);
    }
    assert(!g.unformatted_capacity_bytes());
    expect_throw<std::logic_error>([&] { g.density(0); });
    g.dimensions = Dimensions{20, 50};
    g.raw_bytes_per_track = 272000;
    g.validate();
    assert(g.unformatted_capacity_bytes() == 43520000000ULL);
    const auto outside = g.density(0), inside = g.density(19999);
    near(outside.tracks_per_mm, 20000.0L / 30);
    assert(inside.payload_bits_per_mm > outside.payload_bits_per_mm);
    assert(*outside.raw_bits_per_mm > outside.payload_bits_per_mm);
    near(outside.payload_bits_per_mm2, outside.payload_bits_per_mm * outside.tracks_per_mm);

    const Config c;
    near(c.revolution_ms(), 25.0L / 3);
    near(c.average_rotation_ms(), 25.0L / 6);
    near(c.sector_ms(), 1.0L / 60);
    near(c.average_access_ms(), 551.0L / 60);
    near(c.payload_bytes_per_second(), 30720000);
    near(c.sector_ms() / 64, 0.0002604166666666666667L);
    assert(c.dma_requests_per_sector() == 64);
}

void test_electric_start_and_coasting() {
    HDD disk;
    assert(!disk.ready() && disk.spindle().rpm() == 0);
    expect_throw<std::logic_error>([&] { disk.submit_read({}); });
    disk.advance(100);
    assert(disk.spindle().revolutions() == 0);
    disk.set_power({true, false});
    disk.advance(100);
    assert(!disk.ready() && !disk.spindle().driven());
    expect_throw<std::logic_error>([&] { disk.submit_write({}, Bytes(512)); });
    disk.set_power({false, true});
    disk.advance(100);
    assert(disk.spindle().rpm() == 0); // 无逻辑电源时电机驱动器不使能。

    disk.power_on();
    const auto id = disk.submit_read({}); // 启动期间可排队，但不能提前读到数据。
    disk.advance(500);
    near(disk.spindle().rpm(), 3600);
    near(disk.spindle().revolutions(), 15);
    assert(!disk.controller().active_id());
    disk.advance(500);
    assert(disk.ready() && disk.controller().active_id() == id);
    near(disk.spindle().revolutions(), 60);
    const auto result = completed(disk);
    assert(result.status == Status::Success && result.data == Bytes(512, 0));
    near(result.measurement.queue_ms, 1000);
    check_accounting(result.measurement);
    assert(!disk.head().read_amplifier_enabled() && !disk.head().write_current_enabled());

    disk.power_off();
    assert(!disk.ready() && !disk.spindle().driven());
    disk.advance(250);
    near(disk.spindle().rpm(), 3600);
    disk.advance(250);
    near(disk.spindle().rpm(), 0);
    const auto phase = disk.spindle().phase();
    disk.advance(50);
    near(disk.spindle().phase(), phase);
}

void test_blank_sector_and_dma_priority() {
    HDD disk;
    ready(disk);
    disk.set_bus_signals({true, false}); // CPU 与 DMA 同时请求，DMA 得到下一个周期。
    disk.submit_read({});
    const auto result = completed(disk);
    assert(result.status == Status::Success && result.data == Bytes(512, 0));
    const auto& m = result.measurement;
    near(m.seek_ms, 0);
    near(m.rotation_ms, 0);
    near(m.transfer_ms, 1.0L / 60);
    near(m.drain_ms, disk.config().dma_bus_cycle_ms);
    near(m.media_bytes_per_second(), 30720000, 1e-6L);
    assert(m.media_bytes == 512 && m.dma_bytes == 512);
    assert(m.dma_requests == 64 && m.dma_grants == 64 && m.dma_cpu_contentions == 64);
    assert(disk.media().allocated_sectors() == 0);
    check_accounting(m);
}

void test_magnetic_roundtrip_and_surfaces() {
    HDD disk;
    ready(disk);
    Bytes data(512);
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>(i * 73 + 5);
    const Address address{19999, 7, 499};
    disk.submit_write(address, data);
    const auto write = completed(disk);
    assert(write.status == Status::Success);
    assert(write.measurement.dma_requests == 64 && write.measurement.media_bytes == 512);
    assert(disk.actuator().at(19999) && disk.head().selected() == 7);
    assert(disk.media().allocated_sectors() == 1);
    // 0x05 的低位依次是 1、0、1，NRZI 磁化方向依次为 1、1、0。
    assert(disk.media().inspect_polarity(address, 0));
    assert(disk.media().inspect_polarity(address, 1));
    assert(!disk.media().inspect_polarity(address, 2));
    disk.power_off();
    disk.advance(disk.config().coast_down_ms);
    assert(disk.media().inspect_polarity(address, 0));
    ready(disk);
    disk.submit_read(address);
    const auto read = completed(disk);
    assert(read.status == Status::Success && read.data == data);
    near(read.measurement.seek_ms, 0);

    // 同一柱面上更换磁头只选盘面；不能错误地寻到另一个柱面。
    disk.submit_read({19999, 6, 499});
    const auto other_side = completed(disk);
    assert(other_side.status == Status::Success && other_side.data == Bytes(512, 0));
    near(other_side.measurement.seek_ms, 0);
    assert(disk.head().selected() == 6 && disk.actuator().at(19999));
    disk.submit_read({19998, 7, 499});
    assert(completed(disk).data == Bytes(512, 0));
    disk.submit_read({19999, 7, 498});
    assert(completed(disk).data == Bytes(512, 0));

    // 覆盖写不能把上一次磁化方向误当成数据；所有地址维度都独立。
    Bytes replacement(512, 0xF0);
    disk.submit_write(address, replacement);
    assert(completed(disk).status == Status::Success);
    disk.submit_read(address);
    assert(completed(disk).data == replacement);
    check_accounting(write.measurement);
}

void test_seek_rotation_and_queue() {
    Config config;
    config.controller_ms = 0.2L;
    HDD disk(config);
    ready(disk);
    const auto first_id = disk.submit_read({19999, 7, 250});
    const auto second_id = disk.submit_read({19999, 2, 400});
    assert(disk.controller().queued() == 1);
    assert(disk.run_until_idle(100));
    const auto first = *disk.take_completion(), second = *disk.take_completion();
    assert(first.id == first_id && second.id == second_id);
    near(first.measurement.controller_ms, 0.2L);
    near(first.measurement.seek_ms, 10);
    // 控制器 0.2 ms + 寻道 10 ms 内盘片仍转动，到 sector 250 还差 2.3 ms。
    near(first.measurement.rotation_ms, 2.3L);
    near(second.measurement.seek_ms, 0);
    near(second.measurement.queue_ms, first.measurement.response_ms);
    check_accounting(first.measurement);
    check_accounting(second.measurement);
    assert(disk.statistics().successful_reads == 2);
    near(disk.statistics().mean_access_ms(),
         (first.measurement.access_ms() + second.measurement.access_ms()) / 2);
}

void test_44_measured_average() {
    // 独立采样机械事件：一半为零距离、一半全程寻道，均值正好是标称 5 ms。
    // 在寻道结束时均匀采样 500 个角度的中点，旋转等待均值为半圈。
    Time seek = 0, rotation = 0, transfer = 0, access = 0;
    constexpr unsigned samples = 500;
    const Config config;
    for (unsigned i = 0; i < samples; ++i) {
        HDD disk;
        ready(disk);
        const Time seek_time = i % 2 ? 10 : 0;
        const Time phase_at_arrival = (i + 0.5L) / samples;
        auto phase_before_seek = std::fmod(phase_at_arrival - seek_time / config.revolution_ms(), 1.0L);
        if (phase_before_seek < 0) phase_before_seek += 1;
        disk.advance(phase_before_seek * config.revolution_ms());
        disk.submit_read({i % 2 ? 19999ULL : 0ULL, 0, 0});
        const auto result = completed(disk);
        assert(result.status == Status::Success && result.measurement.dma_requests == 64);
        seek += result.measurement.seek_ms;
        rotation += result.measurement.rotation_ms;
        transfer += result.measurement.transfer_ms;
        access += result.measurement.access_ms();
    }
    near(seek / samples, 5);
    near(rotation / samples, 25.0L / 6);
    near(transfer / samples, 1.0L / 60);
    near(access / samples, 551.0L / 60);
}

void test_dma_stalls_and_errors() {
    HDD disk;
    ready(disk);
    const auto word_ms = disk.config().sector_ms() / 64;
    disk.set_bus_signals({true, true});
    disk.submit_read({});
    disk.advance(word_ms);
    assert(disk.dma().requested() && !disk.dma().owns_bus());
    disk.set_bus_signals({true, false});
    assert(disk.dma().owns_bus());
    disk.set_bus_signals({true, true}); // 也不能抢占已经开始的 DMA 周期。
    disk.advance(disk.config().dma_bus_cycle_ms);
    assert(!disk.dma().requested());
    const auto overrun = completed(disk);
    assert(overrun.status == Status::ReadOverrun);
    assert(overrun.measurement.dma_bytes == 8 && overrun.data == Bytes(8, 0));
    assert(overrun.measurement.dma_requests == 2 && overrun.measurement.dma_grants == 1);
    check_accounting(overrun.measurement);

    disk.submit_write({0, 0, 1}, Bytes(512, 0xA5));
    assert(!disk.run_until_idle(1)); // 第一个写字还在等总线，不能伪造磁记录完成。
    assert(disk.controller().stage() == Stage::Prefetching);
    disk.set_bus_signals({true, false});
    assert(completed(disk).status == Status::Success);

    Config slow_bus;
    slow_bus.dma_bus_cycle_ms = word_ms * 2;
    HDD slow(slow_bus);
    ready(slow);
    slow.submit_write({0, 0, 1}, Bytes(512, 0xA5));
    const auto underrun = completed(slow);
    assert(underrun.status == Status::WriteUnderrun);
    assert(underrun.measurement.media_bytes == 8 && underrun.measurement.dma_bytes == 8);
    assert(underrun.measurement.dma_requests == 2);
    assert(slow.media().allocated_sectors() == 1);
    check_accounting(underrun.measurement);

    // 数据字与总线完成同时到达的边界：应先释放数据寄存器，不产生伪溢出/欠载。
    Config exact_bus;
    exact_bus.dma_bus_cycle_ms = word_ms;
    HDD exact(exact_bus);
    ready(exact);
    exact.submit_write({0, 0, 1}, Bytes(512, 0x5A));
    assert(completed(exact).status == Status::Success);
    exact.submit_read({0, 0, 1});
    assert(completed(exact).data == Bytes(512, 0x5A));
}

void test_power_loss_during_write() {
    HDD disk;
    ready(disk);
    disk.submit_write({0, 0, 1}, Bytes(512, 0xA5));
    const auto queued_id = disk.submit_read({0, 0, 2});
    disk.advance(disk.config().dma_bus_cycle_ms);
    assert(disk.controller().stage() == Stage::Rotating);
    const auto wait = (1.0L / 500 - disk.spindle().phase()) * disk.config().revolution_ms();
    disk.advance(wait);
    assert(disk.controller().stage() == Stage::Transferring && disk.head().write_current_enabled());
    disk.advance(disk.config().sector_ms() / 64 * 3.5L);
    disk.set_power({false, true}); // 只失去逻辑供电同样不能继续产生写电流。
    const auto partial = *disk.take_completion(), queued = *disk.take_completion();
    assert(partial.status == Status::PowerLost && partial.measurement.media_bytes == 24);
    assert(queued.status == Status::PowerLost && queued.id == queued_id);
    assert(!disk.busy() && !disk.dma().requested() && !disk.head().write_current_enabled());
    check_accounting(partial.measurement);
    check_accounting(queued.measurement);
    disk.power_off();
    disk.advance(disk.config().coast_down_ms);
    ready(disk);
    disk.submit_read({0, 0, 1});
    const auto read = completed(disk);
    assert(read.status == Status::Success);
    for (std::size_t i = 0; i < read.data.size(); ++i)
        assert(read.data[i] == (i < 24 ? 0xA5 : 0));
    assert(!disk.take_completion()); // 旧命令不在重启后偷偷重放。
}

void test_small_non_power_of_two_disk() {
    Config config;
    config.geometry = {2, 3, 7, 13, 119, Dimensions{10, 20}};
    config.spin_up_ms = 2;
    config.average_seek_ms = 0;
    config.controller_ms = 0.00001L;
    HDD disk(config);
    ready(disk);
    std::vector<Bytes> reference(static_cast<std::size_t>(disk.geometry().sectors()), Bytes(13, 0));
    std::uint32_t rng = 42;
    for (unsigned i = 0; i < 150; ++i) {
        rng = rng * 1664525u + 1013904223u;
        const auto lba = rng % disk.geometry().sectors();
        auto& bytes = reference[static_cast<std::size_t>(lba)];
        for (std::size_t j = 0; j < bytes.size(); ++j)
            bytes[j] = static_cast<std::uint8_t>((rng >> (j % 24)) + j);
        disk.submit_write_lba(lba, bytes);
        const auto write = completed(disk);
        assert(write.status == Status::Success && write.measurement.dma_requests == 2);
        near(write.measurement.transfer_ms, config.sector_ms()); // 包括显式格式开销。
    }
    for (std::uint64_t lba = 0; lba < disk.geometry().sectors(); ++lba) {
        disk.submit_read_lba(lba);
        const auto result = completed(disk);
        assert(result.status == Status::Success && result.data == reference[static_cast<std::size_t>(lba)]);
        assert(result.measurement.dma_requests == 2 && result.measurement.dma_bytes == 13);
        near(result.measurement.transfer_ms, config.sector_ms());
        check_accounting(result.measurement);
    }
}

void test_step_size_independence_and_simulator() {
    Config config;
    config.spin_up_ms = 2;
    config.controller_ms = 0.02L;
    HDD coarse(config), fine(config);
    for (auto* disk : {&coarse, &fine}) {
        disk->power_on();
        disk->submit_write({1234, 7, 250}, Bytes(512, 0x5A));
        disk->submit_read({1234, 7, 250});
    }
    coarse.advance(50);
    for (unsigned i = 0; i < 50000; ++i) fine.tick();
    assert(!coarse.busy() && !fine.busy());
    near(coarse.spindle().phase(), fine.spindle().phase());
    for (unsigned i = 0; i < 2; ++i) {
        const auto a = *coarse.take_completion(), b = *fine.take_completion();
        assert(a.id == b.id && a.status == b.status && a.data == b.data);
        near(a.measurement.response_ms, b.measurement.response_ms);
        near(a.measurement.seek_ms, b.measurement.seek_ms);
        near(a.measurement.rotation_ms, b.measurement.rotation_ms);
        near(a.measurement.transfer_ms, b.measurement.transfer_ms);
        assert(a.measurement.dma_requests == b.measurement.dma_requests);
    }
    colab::simulation::Simulator<HDD> simulator(fine);
    const auto before = fine.now_ms();
    simulator.step();
    near(fine.now_ms() - before, config.tick_ms);
}

void test_validation() {
    expect_throw<std::invalid_argument>([] { Config c; c.rpm = 0; HDD disk(c); });
    expect_throw<std::invalid_argument>([] { Config c; c.spin_up_ms = 0; HDD disk(c); });
    expect_throw<std::invalid_argument>([] { Config c; c.coast_down_ms = 0; HDD disk(c); });
    expect_throw<std::invalid_argument>([] { Config c; c.dma_buffer_bits = 63; HDD disk(c); });
    expect_throw<std::invalid_argument>([] { Config c; c.dma_buffer_bits = 8192; HDD disk(c); });
    expect_throw<std::invalid_argument>([] { Config c; c.geometry.platters = 0; HDD disk(c); });
    expect_throw<std::invalid_argument>([] { Config c; c.geometry.cylinders = 0; HDD disk(c); });
    expect_throw<std::invalid_argument>([] { Config c; c.average_seek_ms = -1; HDD disk(c); });
    expect_throw<std::invalid_argument>([] {
        Config c; c.geometry.raw_bytes_per_track = 1; HDD disk(c);
    });
    expect_throw<std::invalid_argument>([] {
        Config c; c.geometry.dimensions = Dimensions{50, 20}; HDD disk(c);
    });
    expect_throw<std::overflow_error>([] {
        Config c; c.geometry.platters = std::numeric_limits<std::uint64_t>::max(); HDD disk(c);
    });
    expect_throw<std::invalid_argument>([] {
        Config c; c.rpm = std::numeric_limits<Time>::quiet_NaN(); HDD disk(c);
    });
    HDD disk;
    ready(disk);
    expect_throw<std::invalid_argument>([&] { disk.advance(-1); });
    expect_throw<std::invalid_argument>([&] { disk.advance(std::numeric_limits<Time>::infinity()); });
    expect_throw<std::invalid_argument>([&] { disk.submit_write({}, Bytes(511)); });
    expect_throw<std::out_of_range>([&] { disk.submit_read({20000, 0, 0}); });
    expect_throw<std::out_of_range>([&] { disk.submit_read({0, 8, 0}); });
    expect_throw<std::out_of_range>([&] { disk.submit_read({0, 0, 500}); });
    expect_throw<std::out_of_range>([&] { disk.submit_read_lba(disk.geometry().sectors()); });
    expect_throw<std::out_of_range>([&] { disk.media().inspect_polarity({}, 4096); });
    assert(!disk.busy() && !disk.take_completion() && disk.media().allocated_sectors() == 0);

    Config single;
    single.geometry = {1, 1, 1, 1, {}, {}};
    single.dma_buffer_bits = 8;
    HDD one(single);
    ready(one);
    one.submit_write({}, Bytes{0xFF});
    assert(completed(one).status == Status::Success);
    one.submit_read({});
    const auto result = completed(one);
    assert(result.data == Bytes{0xFF} && result.measurement.dma_requests == 1);
    near(result.measurement.seek_ms, 0);
}

int main() {
    test_geometry_and_reference_metrics();
    test_electric_start_and_coasting();
    test_blank_sector_and_dma_priority();
    test_magnetic_roundtrip_and_surfaces();
    test_seek_rotation_and_queue();
    test_44_measured_average();
    test_dma_stalls_and_errors();
    test_power_loss_during_write();
    test_small_non_power_of_two_disk();
    test_step_size_independence_and_simulator();
    test_validation();
    std::cout << "HDD tests passed (power, magnetic media, mechanics, timing, DMA, exercise 44)\n";
}
