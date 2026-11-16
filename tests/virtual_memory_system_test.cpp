#include "../soc/core/mmu/page_table_port/page_table_port.hpp"
#include "../soc/cache/cache.hpp"
#include "../soc/controller/memory/memory.hpp"
#include "../simulation/memory_access.hpp"

#include <cassert>
#include <iostream>

using namespace colab::soc::core::mmu;
using Bytes = PageTablePort::Bytes;

struct System {
    static constexpr Address root = 0x4000;
    colab::dimm::DIMM dram;
    colab::soc::controller::Memory controller;
    colab::soc::Cache cache;
    colab::soc::core::MMU mmu;
    PageTablePort page_table;
    colab::simulation::MemoryAccess access;

    System()
        : dram({1, {4, {4, {32, 128, 4}}}}), controller(dram),
          cache(controller.reader(), controller.writer(), {8, 2, 64}),
          page_table(controller.reader(), controller.writer()),
          access(mmu, [this](Address pa, std::size_t size) { return cache.read(pa, size); },
                 [this](Address pa, const Bytes& data) { cache.write(pa, data); }) {
        mmu.enable_paging(root, page_table.reader(), page_table.writer());
    }

    void set_pte(Address vpn, PageTableEntry entry) { page_table.writer()(root + vpn * 8, entry); }
    PageTableEntry pte(Address vpn) { return page_table.reader()(root + vpn * 8); }
};

template <typename Function>
void expect_fault(Function function, FaultReason reason) {
    bool threw = false;
    try { function(); }
    catch (const TranslationFault& fault) { threw = true; assert(fault.reason() == reason); }
    assert(threw);
}

void test_five_combinations() {
    for (int scenario = 0; scenario < 5; ++scenario) {
        System system;
        constexpr Address va = 0x1234, pa = 0x2234;
        system.controller.write(pa, {42});
        // 预设 A=1，以便此测试单独检验教材简化的主存访问次数。
        system.set_pte(1, {2, scenario != 4, {true, true, true, true}, true});
        if (scenario == 0 || scenario == 1) system.mmu.translate(va);
        if (scenario == 0 || scenario == 2 || scenario == 4) system.cache.read_byte(pa);
        const auto reads = system.controller.statistics().read_requests;
        const auto cache_accesses = system.cache.statistics().hits() + system.cache.statistics().misses();
        if (scenario == 4) {
            expect_fault([&] { system.access.read(va, 1); }, FaultReason::NotPresent);
            assert(system.controller.statistics().read_requests == reads + 1); // 只读页表。
            assert(system.cache.statistics().hits() + system.cache.statistics().misses() == cache_accesses);
            assert(!system.mmu.last_translation()->physical_address);
            for (const auto& entry : system.mmu.tlb().entries()) assert(!entry.valid);
        } else {
            assert(system.access.read(va, 1).front() == 42);
            const bool tlb_hit = scenario == 0 || scenario == 1;
            const bool cache_hit = scenario == 0 || scenario == 2;
            assert(system.mmu.last_translation()->tlb_hit == tlb_hit);
            assert(system.cache.last_access()->hit == cache_hit);
            const auto expected_reads = static_cast<unsigned>(!tlb_hit) + static_cast<unsigned>(!cache_hit);
            assert(system.controller.statistics().read_requests == reads + expected_reads);
        }
    }
}

void test_page_replacement_and_retry() {
    System system;
    constexpr Address va = 0x1234;
    system.set_pte(1, {1, true, {true, true, false, true}, false, false, 17});
    system.access.write(va, {0xAA});
    assert(system.pte(1).accessed && system.pte(1).dirty);
    assert(system.controller.read(0x1234, 1).front() == 0); // 新值还在写回 Cache。

    // 以下是测试宿主扮演 OS 的步骤，不是 MMU 中的换页策略。
    auto old = system.pte(1);
    old.present = false;
    system.set_pte(1, old);
    system.mmu.invalidate_page(1);
    system.cache.invalidate_range(0x1000, page_size); // 先落盘前清理脏行，再允许页框复用。
    const auto saved_page = system.controller.read(0x1000, page_size);
    assert(saved_page[0x234] == 0xAA);
    Bytes incoming(page_size, 0);
    incoming[0x234] = 0x55;
    system.controller.write(0x1000, incoming);
    system.set_pte(2, {1, true, {true, true, false, true}});
    assert(system.access.read(0x2234, 1).front() == 0x55); // 不得读到旧缓存的 0xAA。

    const auto cache_requests = system.cache.statistics().hits() + system.cache.statistics().misses();
    expect_fault([&] { system.access.read(va, 1); }, FaultReason::NotPresent);
    assert(system.mmu.fault()->virtual_address == va);
    assert(system.cache.statistics().hits() + system.cache.statistics().misses() == cache_requests);
    // 外层选择空闲页框、调入保存的页、更新映射并重试同一虚拟地址。
    system.cache.invalidate_range(0x3000, page_size);
    system.controller.write(0x3000, saved_page);
    old.physical_frame = 3;
    old.present = true;
    old.accessed = false;
    old.dirty = false;
    system.set_pte(1, old);
    system.mmu.invalidate_page(1);
    system.mmu.clear_fault();
    assert(system.access.read(va, 1).front() == 0xAA);
    assert(system.mmu.last_translation()->physical_address == 0x3234 && !system.mmu.fault());
    assert(system.pte(1).accessed && !system.pte(1).dirty && system.pte(1).software_data == 17);
}

void test_cross_page_preflight() {
    System system;
    system.set_pte(1, {1, true, {true, true, false, true}});
    system.set_pte(2, {3, true, {true, true, false, true}});
    Bytes data(32);
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>(i + 1);
    system.access.write(0x1FF0, data);
    assert(system.access.read(0x1FF0, data.size()) == data);
    system.cache.flush();
    assert(system.controller.read(0x1FF0, 16) == Bytes(data.begin(), data.begin() + 16));
    assert(system.controller.read(0x3000, 16) == Bytes(data.begin() + 16, data.end()));
    assert(system.controller.read(0x2000, 16) == Bytes(16, 0)); // 不能假定下一物理页连续。

    auto second = system.pte(2);
    second.permissions.writable = false;
    system.set_pte(2, second);
    system.mmu.invalidate_page(2);
    const auto old_requests = system.cache.statistics().hits() + system.cache.statistics().misses();
    expect_fault([&] { system.access.write(0x1FF0, Bytes(32, 0xFF)); }, FaultReason::WriteDenied);
    assert(system.mmu.fault()->virtual_address == 0x2000);
    assert(system.cache.statistics().hits() + system.cache.statistics().misses() == old_requests);
    assert(system.access.read(0x1FF0, 32) == data); // 第二页权限失败，不得部分写入第一页的数据。
    expect_fault([&] { system.access.fetch(0x1000, 2); }, FaultReason::ExecuteDenied);
}

int main() {
    test_five_combinations();
    test_page_replacement_and_retry();
    test_cross_page_preflight();
    std::cout << "Virtual memory/Cache/DIMM integration tests passed\n";
}
