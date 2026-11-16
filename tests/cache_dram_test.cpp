#include "../soc/cache/cache.hpp"
#include "../soc/controller/memory/memory.hpp"

#include <cassert>
#include <iostream>

using colab::soc::Cache;
using colab::soc::controller::Memory;
using namespace colab::soc::cache;

template <typename Exception, typename Function>
void expect_throw(Function function) {
    bool threw = false;
    try { function(); }
    catch (const Exception&) { threw = true; }
    assert(threw);
}

void test_controller() {
    colab::dimm::DIMM a({2, {4, {4, {2, 4, 4}}}}); // 128 字节，16 位 Rank。
    colab::dimm::DIMM b({1, {8, {2, {2, 4, 8}}}}); // 128 字节，64 位 Rank。
    Memory controller(a, 0x8000);
    controller.map(0x9000, b);
    controller.write(0x8000, {1, 2, 3, 4});
    assert(a.read(0) == 0x0201 && a.read(1) == 0x0403);
    assert(controller.statistics().word_reads == 0 && controller.statistics().word_writes == 2);
    controller.write(0x8001, {0xAB});
    assert(a.read(0) == 0xAB01 && a.read(1) == 0x0403);
    assert(controller.statistics().word_reads == 1);
    assert(controller.read(0x8000, 4) == Bytes({1, 0xAB, 3, 4}));
    for (std::size_t i = 0; i < 4; ++i) {
        const auto decoded = controller.decode(static_cast<Address>(0x8000 + 2 * i));
        assert(decoded.dram.rank == 0 && decoded.dram.bank == i);
        assert(decoded.dram.row == 0 && decoded.dram.column == 0 && decoded.byte_in_word == 0);
    }
    assert(controller.decode(0x8040).dram.rank == 1);
    controller.write(0x803E, {5, 6, 7, 8}); // 跨 Rank，仍是同一 DIMM 窗口。
    assert(controller.read(0x803E, 4) == Bytes({5, 6, 7, 8}));
    controller.write(0x9000, {0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE});
    assert(b.read(0) == 0xFEDCBA9876543210ull);
    controller.write(0x9007, {0x80});
    assert(b.read(0) == 0x80DCBA9876543210ull);
    const auto reads = controller.statistics().read_requests;
    const auto writes = controller.statistics().write_requests;
    expect_throw<std::out_of_range>([&] { controller.read(0, 1); }); // 不能只截低位，误选 a。
    expect_throw<std::out_of_range>([&] { controller.read(0x807F, 2); });
    expect_throw<std::out_of_range>([&] { controller.write(0x807F, {1, 2}); });
    assert(controller.statistics().read_requests == reads && controller.statistics().write_requests == writes);
    assert(controller.read(0, 0).empty());
    expect_throw<std::invalid_argument>([&] { controller.map(0x8070, b); });
    expect_throw<std::invalid_argument>([&] { controller.map(0xA000, a); });
    expect_throw<std::out_of_range>([&] { Memory bad(a, 0xFFFFFFF0); });
    colab::dimm::DIMM narrow({1, {1, {1, {1, 1, 4}}}});
    expect_throw<std::invalid_argument>([&] { Memory bad(narrow); });
}

void test_four_way_mapping() {
    colab::dimm::DIMM dram({2, {4, {4, {64, 128, 4}}}});
    constexpr Address base = 0x10000000;
    Memory controller(dram, base);
    Config config{128, 4, 64};
    Cache cache(controller.reader(), controller.writer(), config);
    const auto& decoder = cache.address_decoder();
    assert(decoder.offset_bits() == 6 && decoder.index_bits() == 7 && decoder.tag_bits() == 19);
    const auto a = decoder.decode(0x1200), b = decoder.decode(0x1234);
    assert(a.block_number == b.block_number && a.tag == b.tag && a.set_index == b.set_index);
    assert(a.block_offset == 0 && b.block_offset == 0x34);
    const auto capacity = decoder.capacity();
    assert(capacity.data_bits == 32 * 1024 * 8 && capacity.tag_bits == 512 * 19);
    assert(capacity.valid_bits == 512 && capacity.dirty_bits == 512 && capacity.replacement_bits == 1024);
    assert(capacity.total_bits() == 273920);

    // 五个主存块固定映射到第 20 组；前四块可占四路，第五块触发组内替换。
    const auto address = [](unsigned tag) { return base + (tag * 128 + 20) * 64; };
    cache.write_byte(address(0), 0xA5);
    assert(cache.last_access()->mapping.set_index == 20 && *cache.last_access()->way == 0);
    assert(controller.statistics().bytes_read == 64 && controller.statistics().word_reads == 32);
    for (std::size_t bank = 0; bank < 4; ++bank) {
        assert(dram.rank(0).dram_chip(0).bank(bank).stats().activations == 1);
        assert(dram.rank(0).dram_chip(0).bank(bank).stats().reads == 8);
    }
    assert(controller.read(address(0), 1).front() == 0); // 写回之前 DRAM 是旧值。
    const auto reads = controller.statistics().read_requests;
    assert(cache.read_byte(address(0) + 1) == 0);
    assert(controller.statistics().read_requests == reads); // 命中不访问 DIMM。
    for (unsigned tag = 1; tag <= 3; ++tag) {
        cache.read_byte(address(tag));
        assert(cache.last_access()->mapping.set_index == 20 && *cache.last_access()->way == tag);
    }
    cache.read_byte(address(4));
    const auto trace = *cache.last_access();
    assert(!trace.hit && *trace.way == 0 && *trace.evicted_block == address(0) && trace.dirty_writeback);
    assert(controller.read(address(0), 1).front() == 0xA5);
    assert(cache.statistics().fills == 5 && cache.statistics().writebacks == 1);
    assert(cache.statistics().memory_read_bytes == 5 * 64 && cache.statistics().memory_write_bytes == 64);
    for (std::size_t way = 0; way < 4; ++way) {
        assert(cache.line(20, way).valid);
        assert(decoder.decode(decoder.block_address(cache.line(20, way).tag, 20)).set_index == 20);
    }
    assert(!cache.line(19, 0).valid && !cache.line(21, 0).valid);
}

void test_write_buffer() {
    colab::dimm::DIMM dram({1, {2, {2, {4, 8, 8}}}});
    Memory controller(dram);
    Config config{2, 2, 4, WritePolicy::WriteThrough, AllocationPolicy::NoWriteAllocate};
    config.write_buffer_entries = 2;
    Cache cache(controller.reader(), controller.writer(), config);
    cache.write_byte(0, 99);
    cache.write_byte(4, 88);
    assert(cache.pending_writes() == 2 && controller.statistics().write_requests == 0);
    cache.write_byte(8, 77); // 满缓冲阻塞，先排出最早的一项。
    assert(cache.pending_writes() == 2 && cache.statistics().write_buffer_stalls == 1);
    assert(controller.read(0, 1).front() == 99 && controller.read(4, 1).front() == 0);
    assert(cache.drain_one_write());
    assert(controller.read(4, 1).front() == 88 && cache.pending_writes() == 1);
    assert(cache.read_byte(8) == 77); // 读缺失先排空缓冲，不能装入旧数据 0。
    assert(cache.pending_writes() == 0);
    cache.write_byte(9, 66); // 写命中先更新 Cache，DRAM 稍后更新。
    assert(cache.read_byte(9) == 66 && controller.read(9, 1).front() == 0);
    cache.flush();
    assert(controller.read(9, 1).front() == 66 && !cache.drain_one_write());
    assert(cache.statistics().memory_writes == 4 && cache.statistics().memory_write_bytes == 4);

    bool fail = true;
    unsigned delivered = 0;
    Cache retry(controller.reader(), [&](Address, const Bytes&) {
        if (fail) throw std::runtime_error("write failure");
        ++delivered;
    }, config);
    retry.write_byte(0, 1);
    expect_throw<std::runtime_error>([&] { retry.drain_one_write(); });
    assert(retry.pending_writes() == 1 && retry.statistics().memory_writes == 0);
    fail = false;
    retry.flush();
    assert(retry.pending_writes() == 0 && delivered == 1);
}

void test_against_flat_memory() {
    // 独立平坦字节数组验证整个 Cache -> 控制器 -> DIMM 链路。
    // 覆盖四种替换、四种写组合、有/无缓冲、非 2 的幂路数和跨行/跨 Rank 操作。
    for (const auto policy : {ReplacementPolicy::LRU, ReplacementPolicy::FIFO,
                              ReplacementPolicy::Random, ReplacementPolicy::LFU}) {
        for (const auto write : {WritePolicy::WriteBack, WritePolicy::WriteThrough}) {
            for (const auto allocation : {AllocationPolicy::WriteAllocate, AllocationPolicy::NoWriteAllocate}) {
                for (const auto buffer : {std::size_t{0}, std::size_t{2}}) {
                    colab::dimm::DIMM dram({2, {4, {4, {4, 8, 4}}}});
                    Memory controller(dram, 0x4000);
                    Config config{4, 3, 16, write, allocation};
                    config.replacement_policy = policy;
                    config.write_buffer_entries = buffer;
                    Cache cache(controller.reader(), controller.writer(), config);
                    Bytes reference(dram.geometry().capacity_bytes(), 0);
                    std::uint32_t state = 42;
                    for (std::size_t i = 0; i < 600; ++i) {
                        state = state * 1664525u + 1013904223u;
                        const auto size = std::size_t{1} + (state >> 16) % 17;
                        const auto offset = state % (reference.size() - size + 1);
                        const auto address = static_cast<Address>(0x4000 + offset);
                        if (i % 3 != 0) {
                            Bytes values(size);
                            for (std::size_t j = 0; j < size; ++j) values[j] = static_cast<std::uint8_t>(state + j);
                            cache.write(address, values);
                            std::copy(values.begin(), values.end(), reference.begin() + offset);
                        } else {
                            assert(cache.read(address, size) == Bytes(reference.begin() + offset, reference.begin() + offset + size));
                        }
                        if (i % 7 == 0) cache.drain_one_write();
                        if (i % 47 == 0) {
                            cache.flush();
                            assert(controller.read(0x4000, reference.size()) == reference);
                        }
                    }
                    cache.invalidate();
                    assert(controller.read(0x4000, reference.size()) == reference);
                }
            }
        }
    }
}

int main() {
    test_controller();
    test_four_way_mapping();
    test_write_buffer();
    test_against_flat_memory();
    std::cout << "Cache/DRAM integration tests passed\n";
}
