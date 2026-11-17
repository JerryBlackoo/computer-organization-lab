#pragma once

#include "../examples/guest/io_driver.hpp"
#include <array>
#include <cassert>
#include <iostream>
#include <set>

namespace io_test {
using namespace colab;
namespace c = devices::common;
using Block = devices::hdd::IOController<devices::HDD>;
using Mode = examples::guest::IoMode;

inline SoCConfig config(soc::RV32Core::Mode mode,
                        soc::RV32Core::Execution execution = soc::RV32Core::Execution::MultiCycle) {
    SoCConfig result;
    result.disk.spin_up_ms = 1;
    result.disk.tick_ms = 0.05L; // One system tick advances the scaled mechanical clock 50us.
    result.cpu_mode = mode;
    result.cpu_execution = execution;
    return result;
}
inline SoCConfig config() {
    return config(soc::RV32Core::Mode::Hardwired, soc::RV32Core::Execution::Pipelined);
}
inline std::array<SoCConfig, 3> cpu_configs() {
    return {config(), config(soc::RV32Core::Mode::Hardwired), config(soc::RV32Core::Mode::Microprogrammed)};
}
inline c::Bytes pattern(std::size_t size = 512) {
    c::Bytes result(size);
    for (std::size_t i = 0; i < size; ++i) result[i] = static_cast<std::uint8_t>(i * 73 + 5);
    return result;
}
inline void seed_hdd(SoC& system, const c::Bytes& data = pattern()) {
    system.disk().power_on(); system.disk().advance(system.disk().config().spin_up_ms);
    system.disk().submit_write_lba(0, data);
    if (!system.disk().run_until_idle(100)) throw std::runtime_error("disk seed timed out");
    const auto result = system.disk().take_completion();
    if (!result || result->status != devices::hdd::Status::Success) throw std::runtime_error("disk seed failed");
}
template<class Predicate> void until(SoC& system, Predicate predicate, std::uint64_t limit = 200000) {
    while (!predicate() && limit--) system.tick();
    if (!predicate()) throw std::runtime_error("I/O test timed out at CPU PC=" + std::to_string(system.cpu().pc()));
}
inline c::BusResult transfer(SoC& system, std::uint32_t address, bool write, std::uint32_t value = 0) {
    const auto tag = system.bus().submit(0, write ? c::BusOperation::Write : c::BusOperation::Read,
        address, 4, write ? c::word_bytes(value) : c::Bytes{});
    until(system, [&] { return system.bus().ready(tag); });
    return system.bus().take(tag);
}
inline void write(SoC& system, std::uint32_t address, std::uint32_t value) {
    const auto result = transfer(system, address, true, value);
    if (result.status != interconnect::pcie::Status::Success) throw std::runtime_error(result.error);
}
inline std::uint32_t read(SoC& system, std::uint32_t address) {
    const auto result = transfer(system, address, false);
    if (result.status != interconnect::pcie::Status::Success) throw std::runtime_error(result.error);
    return c::load32(result.data);
}
inline void run_guest(SoC& system) {
    until(system, [&] { return c::load32(system.memory().read(0x10, 4)) == 1; });
}
inline void configure_nvme(SoC& system) {
    using C = devices::nvme::Controller;
    write(system, SoC::NvmeBase + C::AdminAttributes, 0x30003);
    write(system, SoC::NvmeBase + C::AdminSq, 0x1000);
    write(system, SoC::NvmeBase + C::AdminCq, 0x2000);
    write(system, SoC::NvmeBase + C::Config, 0x460001);
    until(system, [&] { return system.nvme().controller().state() == C::State::Idle; });
}
inline c::Bytes command(std::uint8_t opcode, unsigned cid, std::uint64_t prp = 0, std::uint32_t nsid = 0,
                        std::uint32_t cdw10 = 0, std::uint32_t cdw11 = 0, std::uint32_t cdw12 = 0) {
    c::Bytes bytes(64);
    c::store32(bytes, 0, opcode | (cid << 16)); c::store32(bytes, 4, nsid);
    c::store32(bytes, 24, static_cast<std::uint32_t>(prp)); c::store32(bytes, 28, static_cast<std::uint32_t>(prp >> 32));
    c::store32(bytes, 40, cdw10); c::store32(bytes, 44, cdw11); c::store32(bytes, 48, cdw12);
    return bytes;
}
inline void create_io_queue(SoC& system) {
    configure_nvme(system);
    system.memory().write(0x1000, command(5, 1, 0x4000, 0, 0x30001, 3));
    system.memory().write(0x1040, command(1, 2, 0x3000, 0, 0x30001, 0x10001));
    write(system, SoC::NvmeBase + 0x1000, 2);
    until(system, [&] { return system.nvme().controller().completed_commands() == 2; });
    assert(c::load32(system.memory().read(0x201c, 4)) == 0x10002);
    write(system, SoC::NvmeBase + 0x1004, 2);
}
template<class E, class F> void throws(F operation) {
    bool caught = false;
    try { operation(); } catch (const E&) { caught = true; }
    assert(caught);
}
} // namespace io_test
