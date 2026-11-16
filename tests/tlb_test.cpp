#include "../soc/core/mmu/tlb/tlb.hpp"

#include <cassert>
#include <iostream>

using namespace colab::soc::core::mmu;

template <typename Exception, typename Function>
void expect_throw(Function function) {
    bool threw = false;
    try { function(); }
    catch (const Exception&) { threw = true; }
    assert(threw);
}

void test_fields_and_valid_bit() {
    TLB tlb(2);
    for (const auto& slot : tlb.entries()) {
        assert(!slot.valid && !slot.matches(0)); // 初始 Tag=0 也不能误命中 VPN=0。
        expect_throw<std::logic_error>([&] { slot.page_table_entry(); });
    }
    assert(!tlb.lookup(0));
    PageTableEntry pte{0x40, true, {true, false, true, false}, true, false, 73};
    tlb.insert(0x8C, pte);
    const auto& slot = tlb.entries()[0];
    assert(slot.valid && slot.virtual_page == 0x8C && slot.physical_frame == 0x40);
    assert(slot.permissions.readable && !slot.permissions.writable);
    assert(slot.permissions.executable && !slot.permissions.user_accessible);
    assert(slot.accessed && !slot.dirty);
    assert(slot.matches(0x8C) && !slot.matches(0x8D));
    const auto hit = tlb.lookup(0x8C).value();
    assert(hit.present && hit.physical_frame == 0x40 && hit.software_data == 73);
    assert(hit.accessed && !hit.dirty && !hit.permissions.user_accessible);

    // 重填同一个 Tag 覆盖全部控制状态，不新增重复槽。
    pte.physical_frame = 0x41;
    pte.permissions = {false, true, false, true};
    pte.dirty = true;
    tlb.insert(0x8C, pte);
    assert(tlb.entries()[0].physical_frame == 0x41 && tlb.entries()[0].dirty);
    assert(!tlb.entries()[0].permissions.readable && tlb.entries()[0].permissions.writable);
    assert(!tlb.entries()[1].valid);

    tlb.invalidate(0x8C);
    assert(!slot.valid && slot.virtual_page == 0x8C && slot.physical_frame == 0x41);
    assert(!slot.matches(0x8C) && !tlb.lookup(0x8C)); // 旧 Tag/PFN 仍在，但 V=0。
    assert(pte.present); // 清 TLB 的 V 不改变主存 PTE 的驻留位 P。
    tlb.insert(0, {0, true, {true, true, true, true}});
    assert(tlb.entries()[0].valid && !tlb.entries()[0].accessed && !tlb.entries()[0].dirty);
    assert(tlb.lookup(0)->physical_frame == 0); // 零地址本身完全合法。
    tlb.flush();
    assert(!tlb.lookup(0));
    for (const auto& entry : tlb.entries()) assert(!entry.valid);
}

void test_replacement_and_validation() {
    const PageTableEntry page{3, true, {true, true, true, true}, true, true};
    TLB tlb(2);
    tlb.insert(1, page);
    tlb.insert(2, page);
    assert(tlb.lru_rank(0) == 1 && tlb.lru_rank(1) == 0);
    tlb.lookup(1);
    assert(tlb.lru_rank(0) == 0 && tlb.lru_rank(1) == 1);
    tlb.insert(3, {4, true, {true, false, false, true}});
    assert(tlb.lookup(1) && tlb.lookup(3) && !tlb.lookup(2));
    assert(!tlb.entries()[1].dirty && !tlb.entries()[1].permissions.writable);
    tlb.invalidate(1);
    tlb.insert(4, page); // 优先无效槽，不能替换仍有效的 3。
    assert(tlb.lookup(3) && tlb.lookup(4));

    expect_throw<std::invalid_argument>([&] { tlb.insert(5, {}); });
    expect_throw<std::invalid_argument>([&] { tlb.insert(5, {0x100000, true, {true, true, true, true}}); });
    expect_throw<std::invalid_argument>([&] { tlb.insert(0x100000, page); });
    expect_throw<std::out_of_range>([&] { tlb.lookup(0x100000); });
    expect_throw<std::out_of_range>([&] { tlb.invalidate(0x100000); });
    expect_throw<std::out_of_range>([&] { tlb.lru_rank(2); });
    assert(tlb.lookup(3) && tlb.lookup(4)); // 非法填充不得破坏旧有效项。
    TLBEntry slot;
    expect_throw<std::invalid_argument>([&] { slot.fill(0, {}); });
    assert(!slot.valid);
}

int main() {
    test_fields_and_valid_bit();
    test_replacement_and_validation();
    std::cout << "TLB field/state tests passed\n";
}
