#pragma once

#include "address_decoder/address_decoder.hpp"
#include "set/set.hpp"
#include "write_buffer/write_buffer.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace colab::soc {

// 独立的 Cache 控制逻辑：地址拆分 -> 选组 -> 比较各路 -> 命中或填充 -> 取/写数据。
// 下级只有读/写字节接口，由构造方接线；本对象不持有 CPU、MMU、内存控制器或 DIMM。
class Cache {
public:
    Cache(cache::ReadMemory read_memory, cache::WriteMemory write_memory, cache::Config config = {})
        : decoder_(config), read_memory_(std::move(read_memory)), write_memory_(std::move(write_memory)),
          write_buffer_(config.write_buffer_entries) {
        if (!read_memory_ || !write_memory_) {
            throw std::invalid_argument("cache requires physical memory read/write callbacks");
        }
        sets_.reserve(config.sets);
        for (std::size_t i = 0; i < config.sets; ++i) {
            sets_.emplace_back(config.ways, config.line_bytes, config.replacement_policy,
                               config.random_seed + static_cast<std::uint32_t>(i));
        }
    }

    cache::Bytes read(cache::Address address, std::size_t size) {
        check_range(address, size);
        cache::Bytes result(size);
        std::size_t done = 0;
        while (done < size) {
            // 每轮处理一个块：确定组和 Tag，再在该组内查找命中路。
            const auto parts = decoder_.decode(static_cast<cache::Address>(address + done));
            auto& set = sets_[parts.set_index];
            auto way = set.find(parts.tag);
            cache::AccessTrace trace{static_cast<cache::Address>(address + done), parts, 0, false,
                                     way.has_value(), way, std::nullopt, false};
            if (way) ++statistics_.read_hits;
            else { ++statistics_.read_misses; way = fill(parts, trace); }
            set.touch(*way);
            const auto count = std::min(size - done, config().line_bytes - parts.block_offset);
            const auto& data = set.line(*way).data;
            std::copy_n(data.begin() + parts.block_offset, count, result.begin() + done);
            trace.bytes = count;
            trace.way = way;
            last_access_ = trace;
            done += count;
        }
        return result;
    }

    void write(cache::Address address, const cache::Bytes& data) {
        check_range(address, data.size());
        std::size_t done = 0;
        while (done < data.size()) {
            const auto current = static_cast<cache::Address>(address + done);
            const auto parts = decoder_.decode(current);
            auto& set = sets_[parts.set_index];
            auto way = set.find(parts.tag);
            const auto count = std::min(data.size() - done, config().line_bytes - parts.block_offset);
            cache::AccessTrace trace{current, parts, count, true, way.has_value(), way, std::nullopt, false};
            if (way) ++statistics_.write_hits;
            else ++statistics_.write_misses;

            if (!way && config().allocation_policy == cache::AllocationPolicy::NoWriteAllocate) {
                submit_write(current, cache::Bytes(data.begin() + done, data.begin() + done + count));
                last_access_ = trace;
                done += count;
                continue;
            }
            if (!way) way = fill(parts, trace);
            // 同步写先更新下级；启用写缓冲时先确保请求已入队，再更新本行。
            if (config().write_policy == cache::WritePolicy::WriteThrough) {
                submit_write(current, cache::Bytes(data.begin() + done, data.begin() + done + count));
            }
            auto& line = set.line(*way);
            std::copy_n(data.begin() + done, count, line.data.begin() + parts.block_offset);
            line.dirty = config().write_policy == cache::WritePolicy::WriteBack;
            set.touch(*way);
            trace.way = way;
            last_access_ = trace;
            done += count;
        }
    }

    std::uint8_t read_byte(cache::Address address) { return read(address, 1).front(); }
    void write_byte(cache::Address address, std::uint8_t value) { write(address, {value}); }

    // 无副作用的 Tag/valid 探测，供逐拍控制器决定是否需要发出下级填充请求。
    // 不访问下级、不更新替换状态或命中统计。
    bool contains(cache::Address address, std::size_t size) const {
        check_range(address, size);
        for (std::size_t done = 0; done < size;) {
            const auto parts = decoder_.decode(static_cast<cache::Address>(address + done));
            if (!sets_[parts.set_index].find(parts.tag)) return false;
            done += std::min(size - done, config().line_bytes - parts.block_offset);
        }
        return true;
    }

    // 写穿总线的写更新监听：只更新已驻留字节，不分配行、不回写、不计 CPU 访问。
    // 写回/缓冲写需要另一个所有权协议，不能使用这个接口。
    void snoop_write(cache::Address address, const cache::Bytes& data) {
        if (config().write_policy != cache::WritePolicy::WriteThrough || config().write_buffer_entries != 0)
            throw std::logic_error("write snooping requires unbuffered write-through cache");
        check_range(address, data.size());
        for (std::size_t done = 0; done < data.size();) {
            const auto parts = decoder_.decode(static_cast<cache::Address>(address + done));
            auto& set = sets_[parts.set_index];
            const auto count = std::min(data.size() - done, config().line_bytes - parts.block_offset);
            if (const auto way = set.find(parts.tag)) {
                auto& line = set.line(*way);
                std::copy_n(data.begin() + done, count, line.data.begin() + parts.block_offset);
            }
            done += count;
        }
    }

    // flush 写回所有脏行并保留有效行；invalidate 先写回，再清空有效位。
    // 析构不调用外部回调，结束模拟前由宿主显式 flush。
    void flush() {
        for (std::size_t set = 0; set < config().sets; ++set) {
            for (std::size_t way = 0; way < config().ways; ++way) write_back(set, way);
        }
        drain_writes();
    }

    // 宿主可每个模拟步推进一次写缓冲；不声称一次调用等于一个真实 DRAM 周期。
    bool drain_one_write() {
        return write_buffer_.drain_one([this](cache::Address address, const cache::Bytes& data) {
            write_memory_(address, data);
            ++statistics_.memory_writes;
            statistics_.memory_write_bytes += data.size();
        });
    }
    void drain_writes() { while (drain_one_write()) {} }
    std::size_t pending_writes() const { return write_buffer_.size(); }

    void invalidate() {
        flush();
        for (auto& set : sets_) {
            for (std::size_t way = 0; way < config().ways; ++way) set.line(way).valid = false;
        }
    }

    // 通用的物理范围维护接口：供外层在 DMA/页框复用前使用，不解释虚拟页。
    // 部分覆盖一行时也处理完整行；clean 保留行，invalidate_range 先 clean 再失效。
    void clean_range(cache::Address address, std::size_t size) {
        check_range(address, size);
        if (size == 0) return;
        drain_writes();
        for (std::size_t set = 0; set < config().sets; ++set) {
            for (std::size_t way = 0; way < config().ways; ++way) {
                if (overlaps(set, way, address, size)) write_back(set, way);
            }
        }
        drain_writes();
    }

    void invalidate_range(cache::Address address, std::size_t size) {
        clean_range(address, size);
        if (size == 0) return;
        for (std::size_t set = 0; set < config().sets; ++set) {
            for (std::size_t way = 0; way < config().ways; ++way) {
                if (overlaps(set, way, address, size)) sets_[set].line(way).valid = false;
            }
        }
    }

    const cache::Config& config() const { return decoder_.config(); }
    const cache::AddressDecoder& address_decoder() const { return decoder_; }
    const cache::Statistics& statistics() const { return statistics_; }
    const std::optional<cache::AccessTrace>& last_access() const { return last_access_; }
    std::size_t lru_rank(std::size_t set, std::size_t way) const { return sets_.at(set).lru_rank(way); }
    std::uint64_t frequency(std::size_t set, std::size_t way) const { return sets_.at(set).frequency(way); }
    const cache::Line& line(std::size_t set, std::size_t way) const { return sets_.at(set).line(way); }

private:
    bool overlaps(std::size_t set, std::size_t way, cache::Address address, std::size_t size) const {
        const auto& line = sets_[set].line(way);
        if (!line.valid) return false;
        const auto start = std::uint64_t{decoder_.block_address(line.tag, set)};
        return start < std::uint64_t{address} + size && address < start + config().line_bytes;
    }

    static void check_range(cache::Address address, std::size_t size) {
        const auto available = std::uint64_t{std::numeric_limits<cache::Address>::max()} - address + 1;
        if (size > available) throw std::out_of_range("cache access exceeds 32-bit address space");
    }

    void write_back(std::size_t set, std::size_t way) {
        auto& line = sets_[set].line(way);
        if (!line.valid || !line.dirty) return;
        submit_write(decoder_.block_address(line.tag, set), line.data);
        line.dirty = false;
        ++statistics_.writebacks;
    }

    void submit_write(cache::Address address, const cache::Bytes& data) {
        if (write_buffer_.capacity() == 0) {
            write_memory_(address, data);
            ++statistics_.memory_writes;
            statistics_.memory_write_bytes += data.size();
            return;
        }
        if (write_buffer_.full()) {
            ++statistics_.write_buffer_stalls;
            drain_one_write();
        }
        write_buffer_.enqueue(address, data);
    }

    std::size_t fill(const cache::AddressParts& parts, cache::AccessTrace& trace) {
        // 仅在目标组内选路；旧脏块写回后，按整行大小装入新块并更新 Tag/valid。
        auto& set = sets_[parts.set_index];
        const auto way = set.victim();
        const auto& old_line = set.line(way);
        if (old_line.valid) trace.evicted_block = decoder_.block_address(old_line.tag, parts.set_index);
        trace.dirty_writeback = old_line.valid && old_line.dirty;
        write_back(parts.set_index, way);
        // 保守地先排空写缓冲，避免读到尚未落入主存的旧值；不模拟转发逻辑。
        drain_writes();
        auto data = read_memory_(parts.block_address, config().line_bytes);
        ++statistics_.memory_reads;
        statistics_.memory_read_bytes += data.size();
        if (data.size() != config().line_bytes) {
            throw std::runtime_error("memory callback returned an incomplete cache line");
        }
        auto& line = set.line(way);
        if (line.valid) ++statistics_.evictions;
        line.data = std::move(data);
        line.tag = parts.tag;
        line.dirty = false;
        line.valid = true;
        set.inserted(way);
        ++statistics_.fills;
        return way;
    }

    cache::AddressDecoder decoder_;
    std::vector<cache::Set> sets_;
    cache::ReadMemory read_memory_;
    cache::WriteMemory write_memory_;
    cache::WriteBuffer write_buffer_;
    cache::Statistics statistics_;
    std::optional<cache::AccessTrace> last_access_;
};

} // namespace colab::soc
