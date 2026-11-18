#include "mmu.hpp"
#include "page_table_port/page_table_port.hpp"

#include <cassert>
#include <iostream>

int main() {
    using namespace colab::soc::core::mmu;
    // 本示例只依赖 mmu/ 和标准库。主存数组里保存真实的 8 字节页表项。
    PageTablePort::Bytes memory(32, 0);
    PageTablePort port(
        [&](Address address, std::size_t size) {
            if (address > memory.size() || size > memory.size() - address) throw std::out_of_range("read");
            return PageTablePort::Bytes(memory.begin() + address, memory.begin() + address + size);
        },
        [&](Address address, const PageTablePort::Bytes& bytes) {
            if (address > memory.size() || bytes.size() > memory.size() - address) throw std::out_of_range("write");
            std::copy(bytes.begin(), bytes.end(), memory.begin() + address);
        });
    colab::soc::core::MMU mmu;
    mmu.enable_paging(0, port.reader(), port.writer());
    constexpr Address va = 0x1234;
    try { mmu.translate(va); }
    catch (const TranslationFault&) {
        std::cout << "fault VA=0x" << std::hex << mmu.fault()->virtual_address
                  << ": page not present; no physical data access issued\n";
    }

    // 只示意外层处理器/OS 修复映射；MMU 自己不分配页框、不读磁盘。
    port.writer()(8, {0x40, true, {true, true, false, true}});
    mmu.invalidate_page(1);
    mmu.clear_fault();
    const auto result = mmu.translate_with_info(va);
    assert(result.physical_address == 0x40234 && !result.tlb_hit);
    std::cout << "retry VA=0x" << va << " -> PA=0x" << result.physical_address << " (TLB miss)\n";
    mmu.translate(va, AccessType::Write);
    const auto pte = port.reader()(8);
    assert(mmu.last_translation()->tlb_hit && pte.accessed && pte.dirty);
    std::cout << "write: TLB hit, PTE accessed=" << pte.accessed << " dirty=" << pte.dirty << '\n';
}
