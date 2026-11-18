#include "../soc/shared/mmu/mmu.hpp"
#include "../soc/shared/mmu/page_table_port/page_table_port.hpp"

#include <cassert>
#include <iostream>

using colab::soc::core::MMU;
using namespace colab::soc::core::mmu;

template <typename Exception, typename Function>
void expect_throw(Function function) {
    bool threw = false;
    try { function(); }
    catch (const Exception&) { threw = true; }
    assert(threw);
}

template <typename Function>
void expect_fault(MMU& mmu, Function function, FaultReason reason, TranslationStage stage) {
    bool threw = false;
    try { function(); }
    catch (const TranslationFault& fault) {
        threw = true;
        assert(fault.reason() == reason);
        assert(mmu.fault() && mmu.fault()->virtual_address == fault.address());
        assert(mmu.fault()->access == fault.access() && mmu.fault()->reason == reason);
        assert(mmu.fault()->stage == stage);
        assert(mmu.last_translation()->fault == reason && !mmu.last_translation()->physical_address);
    }
    assert(threw);
}

struct PhysicalMemory {
    PageTablePort::Bytes bytes = PageTablePort::Bytes(8192, 0);
    std::size_t reads = 0, writes = 0;
    bool fail_read = false, fail_write = false, short_read = false;

    PageTablePort port() {
        return PageTablePort(
            [this](Address address, std::size_t size) {
                if (fail_read) throw std::runtime_error("read failed");
                ++reads;
                if (short_read) return PageTablePort::Bytes{};
                if (address > bytes.size() || size > bytes.size() - address) throw std::out_of_range("read");
                return PageTablePort::Bytes(bytes.begin() + address, bytes.begin() + address + size);
            },
            [this](Address address, const PageTablePort::Bytes& data) {
                if (fail_write) throw std::runtime_error("write failed");
                ++writes;
                if (address > bytes.size() || data.size() > bytes.size() - address) throw std::out_of_range("write");
                std::copy(data.begin(), data.end(), bytes.begin() + address);
            });
    }
};

void test_codec() {
    const PageTableEntry entry{0xABCDE, true, {true, true, true, true}, true, true, 0xA1B2C3D4};
    const PageTableEntryCodec::Bytes expected{0x7F, 0xE0, 0xCD, 0xAB, 0xD4, 0xC3, 0xB2, 0xA1};
    assert(PageTableEntryCodec::encode(entry) == expected);
    const auto decoded = PageTableEntryCodec::decode(expected);
    assert(decoded.physical_frame == entry.physical_frame && decoded.software_data == entry.software_data);
    assert(decoded.present && decoded.accessed && decoded.dirty);
    assert(decoded.permissions.readable && decoded.permissions.writable && decoded.permissions.executable
           && decoded.permissions.user_accessible);
    auto malformed = expected;
    malformed[0] |= 0x80;
    expect_throw<PageTableFormatError>([&] { PageTableEntryCodec::decode(malformed); });
    expect_throw<std::out_of_range>([] {
        PageTableEntryCodec::encode({0x100000, true, {true, true, true, true}});
    });
    expect_throw<std::invalid_argument>([] { PageTablePort invalid({}, {}); });
}

void test_access_bits_and_protection() {
    PhysicalMemory memory;
    auto port = memory.port();
    auto read = port.reader();
    auto write = port.writer();
    write(8, {7, true, {true, true, false, true}, false, false, 99});
    MMU mmu;
    mmu.enable_paging(0, read, write);
    const auto before = memory.reads;
    assert(mmu.translate(0x1234) == 0x7234);
    assert(memory.reads == before + 1 && mmu.statistics().access_bit_writes == 1);
    auto pte = read(8);
    assert(pte.accessed && !pte.dirty && pte.software_data == 99);
    const auto reads = memory.reads, writes = memory.writes;
    assert(mmu.translate_with_info(0x1235).tlb_hit);
    assert(memory.reads == reads && memory.writes == writes); // 命中且 A/D 无变化，不访问页表。
    assert(mmu.translate(0x1234, AccessType::Write) == 0x7234);
    assert(mmu.last_translation()->tlb_hit && !mmu.last_translation()->page_walk);
    assert(mmu.last_translation()->dirty_updated && mmu.statistics().access_bit_writes == 2);
    pte = read(8);
    assert(pte.dirty && pte.accessed && pte.software_data == 99);
    assert(mmu.tlb().entries()[0].dirty);

    expect_fault(mmu, [&] { mmu.translate(0x1234, AccessType::Execute); },
                 FaultReason::ExecuteDenied, TranslationStage::PermissionCheck);
    assert(mmu.fault()->tlb_hit && mmu.fault()->privilege == Privilege::User);
    assert(mmu.fault()->virtual_address == 0x1234 && mmu.fault()->page_table_base == 0);
    assert(mmu.translate(0x1234) == 0x7234 && mmu.fault()); // 故障寄存器保留最近故障。
    mmu.clear_fault();
    assert(!mmu.fault());

    write(16, {8, true, {true, false, false, true}});
    expect_fault(mmu, [&] { mmu.translate(0x2000, AccessType::Write); },
                 FaultReason::WriteDenied, TranslationStage::PermissionCheck);
    pte = read(16);
    assert(!pte.accessed && !pte.dirty); // 被拒绝的首次访问不能标记成成功访问。
    expect_fault(mmu, [&] { mmu.translate(0x3000); }, FaultReason::NotPresent, TranslationStage::PermissionCheck);
    assert(!mmu.fault()->tlb_hit && mmu.fault()->page_table_entry_address == 24);
    write(24, {9, true, {true, true, true, true}});
    mmu.invalidate_page(3);
    assert(mmu.translate(0x3000) == 0x9000); // 外层修复页表后重试原访问。

    // OS 清 A 后要失效对应快表项，下一次访问才会再次写回 A。
    pte = read(8);
    pte.accessed = false;
    write(8, pte);
    mmu.invalidate_page(1);
    mmu.translate(0x1234);
    assert(read(8).accessed);
}

void test_page_table_failures() {
    PhysicalMemory memory;
    auto port = memory.port();
    port.writer()(8, {1, true, {true, true, true, true}});
    MMU mmu;
    mmu.enable_paging(0, port.reader(), port.writer());
    memory.fail_read = true;
    expect_fault(mmu, [&] { mmu.translate(0x1000); }, FaultReason::PageTableAccess, TranslationStage::PageTableRead);
    memory.fail_read = false;
    memory.short_read = true;
    expect_fault(mmu, [&] { mmu.translate(0x1000); }, FaultReason::PageTableAccess, TranslationStage::PageTableRead);
    memory.short_read = false;
    memory.fail_write = true;
    expect_fault(mmu, [&] { mmu.translate(0x1000); }, FaultReason::PageTableAccess, TranslationStage::AccessBitsUpdate);
    for (const auto& entry : mmu.tlb().entries()) assert(!entry.valid);
    memory.fail_write = false;
    assert(mmu.translate(0x1000) == 0x1000);
    memory.fail_write = true;
    expect_fault(mmu, [&] { mmu.translate(0x1000, AccessType::Write); },
                 FaultReason::PageTableAccess, TranslationStage::AccessBitsUpdate);
    assert(!mmu.tlb().entries()[0].dirty);
    memory.fail_write = false;
    assert(mmu.translate(0x1000, AccessType::Write) == 0x1000 && port.reader()(8).dirty);

    mmu.invalidate_page(1);
    memory.bytes[8] |= 0x80;
    expect_fault(mmu, [&] { mmu.translate(0x1000); }, FaultReason::InvalidPageTableEntry, TranslationStage::PageTableRead);
    assert(mmu.statistics().invalid_entries == 1 && mmu.statistics().page_table_faults == 4);
    assert(mmu.statistics().page_faults == 0); // 物理页表读写失败也不能冒充页面不驻留。

    // 切换 PTBR 清快表，立即使用新的页表和权限。
    port.writer()(0x1008, {5, true, {true, true, true, true}});
    mmu.enable_paging(0x1000, port.reader(), port.writer());
    assert(mmu.translate(0x1000) == 0x5000 && !mmu.last_translation()->tlb_hit);
}

void test_cross_page_ranges() {
    PhysicalMemory memory;
    auto port = memory.port();
    port.writer()(8, {7, true, {true, true, true, true}});
    port.writer()(16, {3, true, {true, true, true, true}});
    MMU mmu;
    mmu.enable_paging(0, port.reader(), port.writer());
    const auto spans = mmu.translate_range(0x1FF0, 32);
    assert(spans.size() == 2);
    assert(spans[0].virtual_address == 0x1FF0 && spans[0].physical_address == 0x7FF0 && spans[0].size == 16);
    assert(spans[1].virtual_address == 0x2000 && spans[1].physical_address == 0x3000 && spans[1].size == 16);
    port.writer()(16, {});
    mmu.invalidate_page(2);
    expect_fault(mmu, [&] { mmu.translate_range(0x1FF0, 32, AccessType::Write); },
                 FaultReason::NotPresent, TranslationStage::PermissionCheck);
    assert(mmu.fault()->virtual_address == 0x2000 && mmu.fault()->access == AccessType::Write);
    const auto reads = memory.reads;
    assert(mmu.translate_range(0xFFFFFFFF, 0).empty() && memory.reads == reads);
    expect_throw<std::out_of_range>([&] { mmu.translate_range(0xFFFFFFFF, 2); });
    mmu.disable_paging();
    assert(mmu.translate_range(0xFFFFFFFF, 1).front().physical_address == 0xFFFFFFFF);
}

int main() {
    test_codec();
    test_access_bits_and_protection();
    test_page_table_failures();
    test_cross_page_ranges();
    std::cout << "Virtual memory hardware tests passed\n";
}
