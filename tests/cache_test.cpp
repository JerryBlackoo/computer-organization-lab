#include "../soc/cache/cache.hpp"

#include <cassert>

using colab::soc::Cache;
using namespace colab::soc::cache;

template <typename Exception, typename Function>
void expect_throw(Function function) {
    bool threw = false;
    try { function(); }
    catch (const Exception&) { threw = true; }
    assert(threw);
}

struct Memory {
    Bytes bytes = Bytes(256, 0);
    std::size_t reads = 0;
    std::size_t writes = 0;
    bool fail_write = false;
    bool short_read = false;

    Memory() {
        for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::uint8_t>(i);
    }
    ReadMemory reader() {
        return [this](Address address, std::size_t size) {
            ++reads;
            if (short_read) return Bytes{};
            if (address > bytes.size() || size > bytes.size() - address) throw std::out_of_range("memory read");
            return Bytes(bytes.begin() + address, bytes.begin() + address + size);
        };
    }
    WriteMemory writer() {
        return [this](Address address, const Bytes& data) {
            if (fail_write) throw std::runtime_error("memory write failed");
            ++writes;
            if (address > bytes.size() || data.size() > bytes.size() - address) throw std::out_of_range("memory write");
            std::copy(data.begin(), data.end(), bytes.begin() + address);
        };
    }
};

void test_replacement_policies() {
    auto reader = [](Address address, std::size_t size) { return Bytes(size, static_cast<std::uint8_t>(address)); };
    auto writer = [](Address, const Bytes&) {};
    Config config{1, 4, 4};
    Cache lru(reader, writer, config);
    for (const auto block : {1, 2, 3, 4, 1, 2, 5, 1, 2, 3, 4, 5}) lru.read_byte(block * 4);
    assert(lru.statistics().read_hits == 4 && lru.statistics().read_misses == 8);
    for (std::size_t way = 0; way < 4; ++way) {
        const auto tag = lru.line(0, way).tag;
        assert(tag >= 2 && tag <= 5 && lru.lru_rank(0, way) == 5 - tag);
    }
    Cache thrashing(reader, writer, config);
    for (int round = 0; round < 3; ++round) {
        for (unsigned block = 1; block <= 5; ++block) thrashing.read_byte(block * 4);
    }
    assert(thrashing.statistics().read_hits == 0 && thrashing.statistics().read_misses == 15);

    config.ways = 2;
    config.replacement_policy = ReplacementPolicy::FIFO;
    Cache fifo(reader, writer, config);
    for (const auto block : {1, 2, 1, 3}) fifo.read_byte(block * 4);
    assert(*fifo.last_access()->evicted_block == 4); // 命中 1 不改变其最早装入的身份。
    config.replacement_policy = ReplacementPolicy::LFU;
    Cache lfu(reader, writer, config);
    for (const auto block : {1, 1, 1, 2, 3}) lfu.read_byte(block * 4);
    assert(*lfu.last_access()->evicted_block == 8); // 1 虽更久未用，但累计访问更多。
    assert(lfu.frequency(0, 0) == 3);
    Cache tie(reader, writer, config);
    for (const auto block : {1, 2, 3}) tie.read_byte(block * 4);
    assert(*tie.last_access()->evicted_block == 4); // LFU 同频时按 LRU 打破平局。

    config.replacement_policy = ReplacementPolicy::Random;
    config.random_seed = 123;
    Cache random_a(reader, writer, config), random_b(reader, writer, config);
    for (unsigned block = 0; block < 30; ++block) {
        assert(random_a.read_byte(block * 4) == random_b.read_byte(block * 4));
        assert(random_a.last_access()->way == random_b.last_access()->way);
    }

    Statistics stats;
    assert(stats.hit_rate() == 0 && stats.miss_rate() == 0);
    stats.read_hits = 3;
    stats.write_misses = 1;
    assert(stats.hit_rate() == 0.75 && stats.miss_rate() == 0.25);
    assert(stats.average_access_time(1, 100) == 26);
    config.write_policy = WritePolicy::WriteThrough;
    assert(AddressDecoder(config).capacity().dirty_bits == 0);
}

void test_write_buffer_without_devices() {
    Memory memory;
    memory.bytes.assign(256, 0);
    Config config{2, 2, 4, WritePolicy::WriteThrough, AllocationPolicy::NoWriteAllocate};
    config.write_buffer_entries = 2;
    Cache cache(memory.reader(), memory.writer(), config);
    cache.write_byte(0, 99);
    cache.write_byte(4, 88);
    assert(cache.pending_writes() == 2 && memory.writes == 0);
    cache.write_byte(8, 77);
    assert(cache.pending_writes() == 2 && cache.statistics().write_buffer_stalls == 1);
    assert(memory.bytes[0] == 99 && memory.bytes[4] == 0);
    cache.drain_one_write();
    assert(memory.bytes[4] == 88);
    assert(cache.read_byte(8) == 77); // 缺失读先完成旧的缓冲写。
    assert(cache.pending_writes() == 0 && memory.bytes[8] == 77);
    cache.write_byte(9, 66);
    assert(cache.read_byte(9) == 66 && memory.bytes[9] == 0);
    memory.fail_write = true;
    expect_throw<std::runtime_error>([&] { cache.drain_one_write(); });
    assert(cache.pending_writes() == 1);
    memory.fail_write = false;
    cache.flush();
    assert(cache.pending_writes() == 0 && memory.bytes[9] == 66);
}

void test_physical_range_maintenance() {
    Memory memory;
    Cache cache(memory.reader(), memory.writer(), Config{4, 2, 4});
    cache.write_byte(0, 99);
    cache.write_byte(8, 88);
    cache.clean_range(1, 1); // 部分覆盖也写回整行，但保留有效位。
    assert(memory.bytes[0] == 99 && memory.bytes[8] == 8);
    const auto reads = memory.reads;
    assert(cache.read_byte(0) == 99 && memory.reads == reads);
    cache.invalidate_range(1, 1);
    assert(cache.read_byte(8) == 88 && memory.reads == reads); // 其他行不失效。
    assert(cache.read_byte(0) == 99 && memory.reads == reads + 1);
    const auto writes = memory.writes;
    cache.invalidate_range(0, 0);
    assert(memory.writes == writes);
    expect_throw<std::out_of_range>([&] { cache.invalidate_range(0xFFFFFFFF, 2); });
    cache.invalidate_range(8, 4);
    assert(memory.bytes[8] == 88);
}

int main() {
    test_physical_range_maintenance();
    test_write_buffer_without_devices();
    test_replacement_policies();
    Config config{2, 2, 4};
    Memory memory;
    Cache cache(memory.reader(), memory.writer(), config);
    const auto& decoder = cache.address_decoder();
    assert(decoder.capacity_bytes() == 16);
    assert(decoder.offset_bits() == 2 && decoder.index_bits() == 1 && decoder.tag_bits() == 29);
    const auto parts = decoder.decode(0x2F);
    assert(parts.tag == 5 && parts.set_index == 1 && parts.block_offset == 3 && parts.block_address == 0x2C);
    assert(decoder.block_address(parts.tag, parts.set_index) == parts.block_address);

    assert(cache.read_byte(0) == 0); // 冷缺失，装入整个块。
    assert(cache.read_byte(3) == 3); // 空间局部性，命中同一块。
    assert(memory.reads == 1);
    assert(cache.read_byte(8) == 8); // 同组另一条路。
    cache.write_byte(8, 0xAA);
    assert(memory.bytes[8] == 8); // 写回尚未到主存。
    assert(cache.read_byte(0) == 0); // 0 成为 MRU，8 成为 LRU。
    assert(cache.read_byte(16) == 16); // 替换脏块 8，必须写回原地址。
    assert(memory.bytes[8] == 0xAA && memory.writes == 1);
    assert(cache.statistics().writebacks == 1 && cache.statistics().evictions == 1);
    assert(cache.statistics().read_hits == 2 && cache.statistics().read_misses == 3);
    assert(cache.read_byte(4) == 4); // 独立的另一组。
    const auto reads_before_hit = memory.reads;
    assert(cache.read_byte(16) == 16 && memory.reads == reads_before_hit);

    // 跨行访问拆成多个块；统计按所访问的块而非 API 调用数累计。
    Memory cross_memory;
    Cache cross(cross_memory.reader(), cross_memory.writer(), Config{4, 2, 4});
    const Bytes payload{90, 91, 92, 93, 94, 95};
    cross.write(3, payload);
    assert(cross.statistics().write_misses == 3);
    assert(cross.read(3, payload.size()) == payload);
    assert(cross_memory.bytes[3] == 3);
    cross.flush();
    assert(cross_memory.writes == 3);
    assert(Bytes(cross_memory.bytes.begin() + 3, cross_memory.bytes.begin() + 9) == payload);
    cross.flush();
    assert(cross_memory.writes == 3); // 干净行不会重复写回。
    cross.write_byte(3, 42);
    cross.invalidate();
    assert(cross_memory.bytes[3] == 42); // 失效前保留脏数据。
    const auto old_reads = cross_memory.reads;
    assert(cross.read_byte(3) == 42 && cross_memory.reads == old_reads + 1);

    // 写穿 + 不按写分配：写缺失直接写下级；写命中同时更新缓存和下级。
    Memory through_memory;
    Config through_config{2, 2, 4, WritePolicy::WriteThrough, AllocationPolicy::NoWriteAllocate};
    Cache through(through_memory.reader(), through_memory.writer(), through_config);
    through.write_byte(8, 50);
    assert(through_memory.bytes[8] == 50 && through_memory.reads == 0);
    assert(through.statistics().fills == 0 && through.statistics().write_misses == 1);
    assert(through.read_byte(8) == 50);
    through.write_byte(9, 51);
    assert(through_memory.bytes[9] == 51 && through.read_byte(9) == 51);
    through.flush();
    assert(through_memory.writes == 2 && through.statistics().writebacks == 0);

    Memory allocate_memory;
    through_config.allocation_policy = AllocationPolicy::WriteAllocate;
    Cache allocate(allocate_memory.reader(), allocate_memory.writer(), through_config);
    allocate.write_byte(10, 0xBB);
    assert(allocate_memory.reads == 1 && allocate_memory.bytes[10] == 0xBB);
    assert(allocate.read_byte(9) == 9); // 装入的其他字节未被破坏。

    Memory bypass_memory;
    Config bypass_config{1, 1, 4, WritePolicy::WriteBack, AllocationPolicy::NoWriteAllocate};
    Cache bypass(bypass_memory.reader(), bypass_memory.writer(), bypass_config);
    bypass.write_byte(0, 11);
    assert(bypass_memory.bytes[0] == 11 && bypass_memory.reads == 0);
    bypass.read_byte(0);
    bypass.write_byte(0, 12);
    assert(bypass_memory.bytes[0] == 11 && bypass.read_byte(0) == 12);
    bypass.flush();
    assert(bypass_memory.bytes[0] == 12);

    // 下级写失败时，保留原来的有效脏行，不能用新 Tag 覆盖它。
    Memory failing_memory;
    Cache failing(failing_memory.reader(), failing_memory.writer(), Config{1, 1, 4});
    failing.write_byte(0, 0xCC);
    failing_memory.fail_write = true;
    expect_throw<std::runtime_error>([&] { failing.read_byte(4); });
    assert(failing.line(0, 0).valid && failing.line(0, 0).dirty && failing.line(0, 0).tag == 0);
    assert(failing.read_byte(0) == 0xCC);
    failing_memory.fail_write = false;
    failing_memory.short_read = true;
    expect_throw<std::runtime_error>([&] { failing.read_byte(4); });
    assert(failing_memory.bytes[0] == 0xCC && failing.read_byte(0) == 0xCC);
    assert(failing.line(0, 0).tag == 0 && !failing.line(0, 0).dirty);

    expect_throw<std::invalid_argument>([&] { Cache bad(memory.reader(), memory.writer(), Config{3, 2, 4}); });
    expect_throw<std::invalid_argument>([&] { Cache bad(memory.reader(), memory.writer(), Config{1, 0, 4}); });
    expect_throw<std::invalid_argument>([&] { Cache bad(memory.reader(), memory.writer(), Config{1, 1, 0}); });
    expect_throw<std::invalid_argument>([] { AddressDecoder bad(Config{1u << 20, 1, 1u << 20}); });
    expect_throw<std::invalid_argument>([] { Cache bad({}, {}); });
    Cache edge([](Address address, std::size_t size) {
        assert(address == 0xFFFFFFFC && size == 4);
        return Bytes(size, 7);
    }, [](Address, const Bytes&) {}, Config{1, 1, 4});
    assert(edge.read_byte(0xFFFFFFFF) == 7);
    expect_throw<std::out_of_range>([&] { edge.read(0xFFFFFFFF, 2); });
    expect_throw<std::out_of_range>([&] { edge.write(0xFFFFFFFF, {1, 2}); });
    assert(edge.read(0xFFFFFFFF, 0).empty());

}
