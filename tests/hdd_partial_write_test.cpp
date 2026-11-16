#include "../devices/hdd/hdd.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace colab::devices::hdd;
using colab::devices::HDD;

static Completion completed(HDD& disk) {
    if (!disk.run_until_idle(2000)) throw std::runtime_error("disk timed out");
    auto result = disk.take_completion();
    if (!result || result->status != Status::Success) throw std::runtime_error("disk operation failed");
    return *result;
}
static void ready(HDD& disk) {
    disk.power_on(); disk.advance(disk.config().spin_up_ms);
    if (!disk.ready()) throw std::runtime_error("disk not ready");
}
static bool interrupted(const Bytes& old, const Bytes& payload, unsigned words) {
    HDD disk; ready(disk);
    const Address address{0, 0, 1};
    disk.submit_write(address, old); completed(disk);
    disk.submit_write(address, payload);
    disk.advance(disk.config().dma_bus_cycle_ms);
    if (disk.controller().stage() != Stage::Rotating) throw std::runtime_error("expected rotation");
    auto fraction = 1.0L / disk.geometry().sectors_per_track - disk.spindle().phase();
    if (fraction < 0) fraction += 1;
    disk.advance(fraction * disk.config().revolution_ms());
    if (disk.controller().stage() != Stage::Transferring) throw std::runtime_error("expected transfer");
    disk.advance(disk.config().sector_ms() / 64 * (words + 0.5L));
    disk.power_off();
    auto partial = disk.take_completion();
    if (!partial || partial->status != Status::PowerLost || partial->measurement.media_bytes != words * 8)
        throw std::runtime_error("unexpected committed byte count");
    disk.advance(disk.config().coast_down_ms); ready(disk);
    disk.submit_read(address); const auto read = completed(disk);
    Bytes expected = old;
    std::copy_n(payload.begin(), words * 8, expected.begin());
    if (read.data == expected) return true;
    const auto at = std::mismatch(read.data.begin(), read.data.end(), expected.begin()).first - read.data.begin();
    std::cerr << "FAIL partial words=" << words << " byte=" << at << " actual=" << unsigned(read.data[at])
              << " expected=" << unsigned(expected[at]) << '\n';
    return false;
}
int main() {
    bool ok = true;
    for (unsigned words : {0u, 1u, 3u, 17u, 63u}) {
        for (unsigned pattern = 0; pattern < 4; ++pattern) {
            Bytes old(512, pattern & 2 ? 0x6d : 0);
            Bytes data(512, 0); data[0] = pattern & 1 ? 3 : 1;
            ok = interrupted(old, data, words) && ok;
        }
    }
    HDD disk; ready(disk);
    Bytes data(512); for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>(i * 73 + 1);
    disk.submit_write_lba(0, data); completed(disk);
    disk.submit_read_lba(0); if (completed(disk).data != data) throw std::runtime_error("full round trip failed");
    if (!ok) return 1;
    std::cout << "PASS HDD 20 interrupted writes and full sector round trip\n";
}
