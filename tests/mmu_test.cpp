#include "../soc/core/mmu/mmu.hpp"

#include <cassert>
#include <limits>
#include <map>

using colab::soc::core::MMU;
using namespace colab::soc::core::mmu;

template <typename Function>
void expect_fault(Function function, FaultReason reason, Address address, AccessType access) {
    bool threw = false;
    try { function(); }
    catch (const TranslationFault& fault) {
        threw = true;
        assert(fault.reason() == reason);
        assert(fault.address() == address);
        assert(fault.access() == access);
    }
    assert(threw);
}

template <typename Exception, typename Function>
void expect_throw(Function function) {
    bool threw = false;
    try { function(); }
    catch (const Exception&) { threw = true; }
    assert(threw);
}

int main() {
    MMU mmu;
    assert(!mmu.paging_enabled());
    assert(mmu.translate(0xFFFFFFFF) == 0xFFFFFFFF);
    assert(mmu.statistics().page_walks == 0);

    constexpr Address base = 0x1000;
    std::map<Address, PageTableEntry> table;
    std::size_t reads = 0;
    auto reader = [&](Address address) {
        ++reads;
        assert(address >= base && (address - base) % PageTableWalker::entry_bytes == 0);
        const auto found = table.find(address);
        return found == table.end() ? PageTableEntry{} : found->second;
    };
    auto map_page = [&](Address vpn, PageTableEntry entry) {
        table[base + vpn * PageTableWalker::entry_bytes] = entry;
    };
    map_page(0x8C, {0x40, true, {true, false, false, true}});
    mmu.enable_paging(base, reader);

    // 图中的虚页 0008CH -> 实页 0040H，页内偏移不变。
    auto result = mmu.translate_with_info(0x0008C123);
    assert(result.physical_address == 0x00040123);
    assert(result.virtual_page == 0x8C && result.physical_frame == 0x40);
    assert(result.page_offset == 0x123 && result.paging_enabled && !result.tlb_hit);
    result = mmu.translate_with_info(0x0008CFFF);
    assert(result.physical_address == 0x00040FFF && result.tlb_hit);
    assert(reads == 1);

    // 已在 TLB 中的只读页：命中也不能越权写或执行。
    expect_fault([&] { mmu.translate(0x8C123, AccessType::Write); },
                 FaultReason::WriteDenied, 0x8C123, AccessType::Write);
    expect_fault([&] { mmu.translate(0x8C123, AccessType::Execute); },
                 FaultReason::ExecuteDenied, 0x8C123, AccessType::Execute);
    assert(reads == 1 && mmu.statistics().protection_faults == 2);

    map_page(1, {3, true, {true, true, false, false}});
    expect_fault([&] { mmu.translate(0x1000); },
                 FaultReason::PrivilegeViolation, 0x1000, AccessType::Read);
    assert(mmu.translate(0x1000, AccessType::Read, Privilege::Supervisor) == 0x3000);
    expect_fault([&] { mmu.translate(0x1000); },
                 FaultReason::PrivilegeViolation, 0x1000, AccessType::Read);
    expect_fault([&] { mmu.translate(0x1000, AccessType::Execute, Privilege::Supervisor); },
                 FaultReason::ExecuteDenied, 0x1000, AccessType::Execute);

    // 未命中不是缺页；只有页表项 present=0 才报告 NotPresent。
    expect_fault([&] { mmu.translate(0x2001); },
                 FaultReason::NotPresent, 0x2001, AccessType::Read);
    assert(mmu.statistics().page_faults == 1);
    map_page(2, {5, true, {true, true, true, true}});
    assert(mmu.translate(0x2001) == 0x5001); // 缺页结果未装入 TLB。

    // 修改页表后须失效对应 TLB 项，才能看到新映射/新权限。
    map_page(2, {6, true, {true, false, false, true}});
    assert(mmu.translate(0x2001, AccessType::Write) == 0x5001);
    mmu.invalidate_page(2);
    expect_fault([&] { mmu.translate(0x2001, AccessType::Write); },
                 FaultReason::WriteDenied, 0x2001, AccessType::Write);
    assert(mmu.translate(0x2001) == 0x6001);

    map_page(4, {0x100000, true, {true, true, true, true}});
    expect_fault([&] { mmu.translate(0x4000); },
                 FaultReason::InvalidPageTableEntry, 0x4000, AccessType::Read);
    map_page(5, {6, true, {false, true, false, true}});
    expect_fault([&] { mmu.translate(0x5000); },
                 FaultReason::ReadDenied, 0x5000, AccessType::Read);
    assert(mmu.translate(0x5000, AccessType::Write) == 0x6000);

    // 两项全相联 LRU：访问 1、2、1、3 后，2 被替换。
    MMU small(2);
    std::size_t walks = 0;
    small.enable_paging(0, [&](Address pte_address) {
        ++walks;
        return PageTableEntry{pte_address / 8 + 10, true, {true, true, true, true}};
    });
    small.translate(0x1000);
    small.translate(0x2000);
    small.translate(0x1000);
    small.translate(0x3000);
    assert(walks == 3);
    small.translate(0x2000);
    assert(walks == 4 && small.statistics().tlb_hits == 1);
    small.flush_tlb();
    small.translate(0x2000);
    assert(walks == 5);

    small.enable_paging(0x8000, [](Address address) {
        assert(address == 0x8010);
        return PageTableEntry{9, true, {true, true, true, true}};
    });
    assert(small.translate(0x2000) == 0x9000); // 切换页表已清 TLB。
    small.disable_paging();
    assert(small.translate(0x2000) == 0x2000);

    PageTableWalker walker;
    expect_throw<std::logic_error>([&] { walker.walk(0); });
    expect_throw<std::invalid_argument>([&] { walker.configure(1, reader); });
    expect_throw<std::invalid_argument>([&] { walker.configure(0, {}); });
    walker.configure(0xFFFFFFF8, reader);
    expect_throw<std::out_of_range>([&] { walker.walk(1); });
    expect_throw<std::invalid_argument>([] { MMU invalid(0); });
    assert(AddressTranslator::compose(max_page_number, page_size - 1) == 0xFFFFFFFF);
    expect_throw<std::out_of_range>([] { AddressTranslator::compose(0x100000, 0); });

}
