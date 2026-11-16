#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

namespace colab::devices { class HDD; }

namespace colab::devices::hdd {

using Bytes = std::vector<std::uint8_t>;
using Time = long double; // 所有时间统一为模拟毫秒，不使用宿主 sleep/墙钟。

inline void check_time(Time time) {
    if (!std::isfinite(time) || time < 0) throw std::invalid_argument("invalid HDD time");
}

struct Address {
    std::uint64_t cylinder = 0; // 所有盘面上半径相同的磁道组成一个柱面。
    std::uint64_t head = 0;     // 一个记录面对应一个磁头。
    std::uint64_t sector = 0;   // 本教学模型的 C/H/S 均从 0 编号。
};

enum class Operation { Read, Write };
enum class Stage { Idle, Decoding, Prefetching, Seeking, Rotating, Transferring, Draining };
enum class Status { Success, PowerLost, ReadOverrun, WriteUnderrun };

struct Power {
    bool logic_5v = false; // 控制器、读写放大器的供电有效信号。
    bool motor_12v = false; // 主轴/音圈驱动器的供电有效信号。
    bool operational() const { return logic_5v && motor_12v; }
};

struct BusSignals {
    bool cpu_request = false;
    bool cpu_cycle_in_progress = false; // 不能抢占已经开始的主存总线周期。
};

struct Measurement {
    Time queue_ms = 0;
    Time controller_ms = 0;
    Time prefetch_ms = 0;
    Time seek_ms = 0;
    Time rotation_ms = 0;
    Time transfer_ms = 0;
    Time drain_ms = 0;
    Time response_ms = 0;
    std::uint64_t dma_requests = 0;
    std::uint64_t dma_grants = 0;
    std::uint64_t dma_cpu_contentions = 0;
    std::uint64_t dma_bytes = 0;
    std::uint64_t media_bytes = 0;

    Time access_ms() const { return seek_ms + rotation_ms + transfer_ms; }
    Time media_bytes_per_second() const { return transfer_ms ? media_bytes * 1000.0L / transfer_ms : 0; }
    Time host_bytes_per_second() const { return response_ms ? dma_bytes * 1000.0L / response_ms : 0; }
};

struct Completion {
    std::uint64_t id = 0;
    Address address{};
    Operation operation = Operation::Read;
    Status status = Status::Success;
    Bytes data; // Read 的主存目的区域；失败时只返回已由 DMA 搬入的前缀。
    Measurement measurement{};
};

struct Statistics {
    std::uint64_t successful_reads = 0;
    std::uint64_t successful_writes = 0;
    std::uint64_t failed_commands = 0;
    std::uint64_t media_bytes_read = 0;
    std::uint64_t media_bytes_written = 0;
    Time successful_access_ms = 0;
    Time successful_response_ms = 0;

    Time mean_access_ms() const {
        const auto count = successful_reads + successful_writes;
        return count ? successful_access_ms / count : 0;
    }
    Time mean_response_ms() const {
        const auto count = successful_reads + successful_writes;
        return count ? successful_response_ms / count : 0;
    }
};

} // namespace colab::devices::hdd
