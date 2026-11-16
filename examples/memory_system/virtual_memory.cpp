#include "../../soc/cache/cache.hpp"
#include "../../soc/controller/memory/memory.hpp"
#include "../../soc/core/mmu/page_table_port/page_table_port.hpp"
#include "../../simulation/memory_access.hpp"

#include <iostream>

void demonstrate(int scenario) {
    using namespace colab::soc::core::mmu;
    colab::dimm::DIMM dram({1, {4, {4, {32, 128, 4}}}});
    colab::soc::controller::Memory controller(dram);
    colab::soc::Cache cache(controller.reader(), controller.writer());
    PageTablePort table(controller.reader(), controller.writer());
    colab::soc::core::MMU mmu;
    constexpr Address root = 0x4000, va = 0x1234, pa = 0x2234;
    table.writer()(root + 8, {2, scenario != 5, {true, true, false, true}, true});
    mmu.enable_paging(root, table.reader(), table.writer());
    controller.write(pa, {42});
    colab::simulation::MemoryAccess access(mmu,
        [&](Address address, std::size_t size) { return cache.read(address, size); },
        [&](Address address, const PageTablePort::Bytes& data) { cache.write(address, data); });

    if (scenario == 1 || scenario == 2) mmu.translate(va);
    if (scenario == 1 || scenario == 3) cache.read_byte(pa);
    const auto reads = controller.statistics().read_requests;
    try {
        const auto value = access.read(va, 1).front();
        std::cout << "case " << scenario << ": TLB " << (mmu.last_translation()->tlb_hit ? "hit" : "miss")
                  << ", Page resident, Cache " << (cache.last_access()->hit ? "hit" : "miss")
                  << ", value=" << static_cast<unsigned>(value)
                  << ", memory read requests=" << controller.statistics().read_requests - reads << '\n';
    } catch (const TranslationFault&) {
        std::cout << "case 5: TLB miss, Page not present, Cache skipped; fault VA=0x"
                  << std::hex << mmu.fault()->virtual_address << std::dec << '\n';
        // 外层示意 OS 在另一个页框调入内容、修改页表；MMU 仅提供故障状态和重试能力。
        cache.invalidate_range(0x3000, page_size);
        controller.write(0x3234, {99});
        table.writer()(root + 8, {3, true, {true, true, false, true}});
        mmu.invalidate_page(1);
        mmu.clear_fault();
        const auto value = access.read(va, 1).front();
        std::cout << "outer handler repaired PTE; retry same VA -> PA=0x" << std::hex
                  << *mmu.last_translation()->physical_address << std::dec
                  << ", value=" << static_cast<unsigned>(value) << '\n';
    }
    cache.flush();
}

int main() {
    for (int scenario = 1; scenario <= 5; ++scenario) demonstrate(scenario);
}
