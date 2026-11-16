#include "../../soc/cache/cache.hpp"
#include "../../soc/controller/memory/memory.hpp"

#include <iostream>

int main() {
    using namespace colab::soc::cache;
    // 两个 Rank、每 Rank 四颗 x4 芯片、每芯片四个 Bank，共 128 KiB。
    colab::dimm::DIMM dram({2, {4, {4, {64, 128, 4}}}});
    constexpr Address base = 0x10000000;
    colab::soc::controller::Memory controller(dram, base);
    colab::soc::Cache cache(controller.reader(), controller.writer(), Config{128, 4, 64});
    const auto address = [](unsigned tag) { return base + (tag * 128 + 20) * 64; };
    const auto print = [&] {
        const auto& trace = *cache.last_access();
        std::cout << "PA=0x" << std::hex << trace.address << std::dec
                  << " block=" << trace.mapping.block_number << " set=" << trace.mapping.set_index
                  << " tag=" << trace.mapping.tag << " offset=" << trace.mapping.block_offset
                  << " way=" << *trace.way << ' ' << (trace.hit ? "HIT" : "MISS");
        if (trace.evicted_block) {
            std::cout << " evicted=0x" << std::hex << *trace.evicted_block << std::dec
                      << " dirty_writeback=" << trace.dirty_writeback;
        }
        std::cout << '\n';
    };

    cache.write_byte(address(0), 0xA5); // 第 20 组第 0 路，写回策略先留在 Cache。
    print();
    cache.read_byte(address(0) + 1); // 同一主存块的另一个字节，无需访问 DRAM。
    print();
    for (unsigned tag = 1; tag <= 4; ++tag) {
        cache.read_byte(address(tag));
        print();
    }
    const auto value = controller.read(address(0), 1).front();
    std::cout << "evicted dirty data in DRAM=" << static_cast<unsigned>(value) << '\n';
    const auto decoded = controller.decode(address(0));
    std::cout << "DRAM rank=" << decoded.dram.rank << " bank=" << decoded.dram.bank
              << " row=" << decoded.dram.row << " column=" << decoded.dram.column
              << " byte_in_word=" << decoded.byte_in_word << '\n';
    const auto capacity = cache.address_decoder().capacity();
    std::cout << "Cache data bytes=" << capacity.data_bits / 8
              << " total bits (including metadata)=" << capacity.total_bits()
              << " hit rate=" << cache.statistics().hit_rate() << '\n';
    cache.flush();
}
