#include "../../soc/cache/cache.hpp"
#include "../../soc/core/mmu/mmu.hpp"

#include <iostream>
#include <stdexcept>

int main() {
    using namespace colab::soc::cache;
    using namespace colab::soc::core::mmu;

    Bytes memory(1024 * 1024, 0);
    memory[0x40123] = 42;
    colab::soc::Cache cache(
        [&](Address pa, std::size_t size) {
            if (pa > memory.size() || size > memory.size() - pa) throw std::out_of_range("memory read");
            return Bytes(memory.begin() + pa, memory.begin() + pa + size);
        },
        [&](Address pa, const Bytes& bytes) {
            if (pa > memory.size() || bytes.size() > memory.size() - pa) throw std::out_of_range("memory write");
            std::copy(bytes.begin(), bytes.end(), memory.begin() + pa);
        });

    colab::soc::core::MMU mmu;
    constexpr Address page_table_base = 0x80000;
    // 页表读接口用一个已解码的页表项模拟；实际主存接入时由接口解码 PTE 字节。
    mmu.enable_paging(page_table_base, [](Address pte_address) {
        if (pte_address == page_table_base + 0x8C * PageTableWalker::entry_bytes) {
            return PageTableEntry{0x40, true, {true, false, false, true}};
        }
        return PageTableEntry{};
    });

    constexpr Address va = 0x0008C123;
    for (int i = 0; i < 2; ++i) {
        const auto translation = mmu.translate_with_info(va, AccessType::Read);
        const auto value = cache.read_byte(translation.physical_address);
        std::cout << "VA=0x" << std::hex << va << " -> PA=0x" << translation.physical_address
                  << std::dec << ", TLB " << (translation.tlb_hit ? "hit" : "miss")
                  << ", value=" << static_cast<unsigned>(value) << '\n';
    }
    try {
        // TLB 已命中，但页面没有写权限，因此不会执行 Cache 的写入。
        cache.write_byte(mmu.translate(va, AccessType::Write), 99);
    } catch (const TranslationFault& fault) {
        std::cout << "write blocked: " << fault.what() << '\n';
    }
    std::cout << "Cache read hits=" << cache.statistics().read_hits
              << ", read misses=" << cache.statistics().read_misses << '\n';
    cache.flush();
}
