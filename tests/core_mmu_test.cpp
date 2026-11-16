#include "../soc/core/core.hpp"

#include <cassert>

using namespace colab::soc::core::mmu;

template <typename Function>
void expect_fault(Function function, FaultReason reason, Address address, AccessType access) {
    bool threw = false;
    try { function(); }
    catch (const TranslationFault& fault) {
        threw = true;
        assert(fault.reason() == reason && fault.address() == address && fault.access() == access);
    }
    assert(threw);
}

void test_execute_permission() {
    // Core 发的是 Execute 请求，不能拿 readable 权限代替 executable。
    colab::soc::Core core({{colab::isa::Opcode::Halt}});
    PageTableEntry instruction_page{0, true, {true, false, false, true}};
    core.mmu().enable_paging(0, [&](Address) { return instruction_page; });
    core.tick();
    expect_fault([&] { core.tick(); }, FaultReason::ExecuteDenied, 0, AccessType::Execute);
    assert(core.phase() == colab::soc::core::control_unit::Phase::FetchRead);
    assert(core.registers().pc.read() == 0 && core.registers().mdr.read() == 0);
    instruction_page.permissions.executable = true;
    for (int i = 0; i < 7; ++i) core.tick();
    assert(core.is_halted());
}

void test_page_fault_restart() {
    colab::soc::Core core({{colab::isa::Opcode::Halt}});
    PageTableEntry page{0, false, {false, false, true, true}};
    core.mmu().enable_paging(0, [&](Address) { return page; },
        [&](Address, const PageTableEntry& updated) { page = updated; });
    core.tick();
    expect_fault([&] { core.tick(); }, FaultReason::NotPresent, 0, AccessType::Execute);
    assert(core.mmu().fault()->virtual_address == 0);
    assert(core.phase() == colab::soc::core::control_unit::Phase::FetchRead);
    assert(core.registers().pc.read() == 0 && core.registers().ir.read() == 0);
    page.present = true;
    core.mmu().invalidate_page(0);
    core.mmu().clear_fault();
    for (int i = 0; i < 7; ++i) core.tick();
    assert(core.is_halted() && core.registers().pc.read() == 2);
    assert(page.accessed && !page.dirty);
}

int main() {
    test_execute_permission();
    test_page_fault_restart();
}
