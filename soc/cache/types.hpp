#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace colab::soc::cache {

using Address = std::uint32_t;
using Bytes = std::vector<std::uint8_t>;
// Cache 与外界的唯一数据边界：物理字节地址、长度/字节数组。
// 下级可以是测试数组、另一层 Cache 或外部控制器，由调用方选择。
using ReadMemory = std::function<Bytes(Address, std::size_t)>;
using WriteMemory = std::function<void(Address, const Bytes&)>;

enum class WritePolicy { WriteBack, WriteThrough };
enum class AllocationPolicy { WriteAllocate, NoWriteAllocate };
enum class ReplacementPolicy { LRU, FIFO, Random, LFU };

struct Config {
    std::size_t sets = 16;
    std::size_t ways = 2;
    std::size_t line_bytes = 32;
    WritePolicy write_policy = WritePolicy::WriteBack;
    AllocationPolicy allocation_policy = AllocationPolicy::WriteAllocate;
    ReplacementPolicy replacement_policy = ReplacementPolicy::LRU;
    std::uint32_t random_seed = 1;
    std::size_t write_buffer_entries = 0; // 0 为同步直接写；非零时由宿主推进写缓冲。
};

struct AddressParts {
    Address tag;
    std::size_t set_index;
    std::size_t block_offset;
    Address block_address;
    Address block_number;
};

struct AccessTrace {
    Address address;
    AddressParts mapping;
    std::size_t bytes;
    bool write;
    bool hit;
    std::optional<std::size_t> way; // 非写分配的写缺失没有占用任何路。
    std::optional<Address> evicted_block;
    bool dirty_writeback;
};

// 逻辑硬件位数，不是 C++ 对象的 sizeof 或 vector 的宿主内存开销。
struct Capacity {
    std::uint64_t data_bits;
    std::uint64_t tag_bits;
    std::uint64_t valid_bits;
    std::uint64_t dirty_bits;
    std::uint64_t replacement_bits;
    std::uint64_t array_bits() const { return data_bits + tag_bits + valid_bits + dirty_bits; }
    std::uint64_t total_bits() const { return array_bits() + replacement_bits; }
};

struct Statistics {
    std::uint64_t read_hits = 0;
    std::uint64_t read_misses = 0;
    std::uint64_t write_hits = 0;
    std::uint64_t write_misses = 0;
    std::uint64_t fills = 0;
    std::uint64_t evictions = 0;
    std::uint64_t writebacks = 0;
    std::uint64_t memory_reads = 0, memory_writes = 0;
    std::uint64_t memory_read_bytes = 0, memory_write_bytes = 0;
    std::uint64_t write_buffer_stalls = 0;

    std::uint64_t hits() const { return read_hits + write_hits; }
    std::uint64_t misses() const { return read_misses + write_misses; }
    double hit_rate() const {
        const auto count = hits() + misses();
        return count == 0 ? 0.0 : static_cast<double>(hits()) / static_cast<double>(count);
    }
    double miss_rate() const {
        const auto count = hits() + misses();
        return count == 0 ? 0.0 : static_cast<double>(misses()) / static_cast<double>(count);
    }
    // 串行查 Cache，再在 miss 时付出额外代价的理论估计；不代表逐拍计时。
    double average_access_time(double hit_time, double miss_penalty) const {
        return hit_time + miss_rate() * miss_penalty;
    }
};

} // namespace colab::soc::cache
