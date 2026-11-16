#include "cache.hpp"

#include <cassert>
#include <iostream>

int main() {
    using namespace colab::soc::cache;

    // 仅用字节数组充当下级：这个示例只依赖 cache/ 和 C++ 标准库。
    Bytes memory(64, 0);
    std::size_t lower_reads = 0;
    colab::soc::Cache cache(
        [&](Address address, std::size_t size) {
            if (address > memory.size() || size > memory.size() - address) throw std::out_of_range("read");
            ++lower_reads;
            return Bytes(memory.begin() + address, memory.begin() + address + size);
        },
        [&](Address address, const Bytes& data) {
            if (address > memory.size() || data.size() > memory.size() - address) throw std::out_of_range("write");
            std::copy(data.begin(), data.end(), memory.begin() + address);
        }, Config{2, 2, 4}); // 两组、两路、每行四字节，使用默认 LRU + 写回。

    const auto show_access = [&] {
        const auto& access = *cache.last_access();
        std::cout << "address=" << access.address << " block=" << access.mapping.block_number
                  << " set=" << access.mapping.set_index << " tag=" << access.mapping.tag
                  << " offset=" << access.mapping.block_offset << " way=" << *access.way
                  << ' ' << (access.hit ? "HIT" : "MISS");
        if (access.evicted_block) std::cout << " evicted=" << *access.evicted_block;
        std::cout << " writeback=" << access.dirty_writeback << '\n';
    };

    cache.write_byte(0, 42); // 块 0 -> 第 0 组，装入一路并置脏。
    show_access();
    assert(memory[0] == 0);
    assert(cache.read_byte(1) == 0); // 同一块的另一字节命中，无需读下级。
    show_access();
    assert(lower_reads == 1);
    cache.read_byte(8); // 块 2 -> 第 0 组，占另一路。
    show_access();
    cache.read_byte(16); // 块 4 -> 第 0 组，替换最久未用的脏块 0。
    show_access();
    assert(memory[0] == 42);
    assert(cache.last_access()->dirty_writeback);
    cache.flush();
    std::cout << "lower memory[0]=" << static_cast<unsigned>(memory[0]) << '\n';
}
