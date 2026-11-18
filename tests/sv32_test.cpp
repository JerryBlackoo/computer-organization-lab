#include "../soc/rv32/core.hpp"
#include <iostream>
using namespace colab::soc::core;
using Access = mmu::AccessType;
using Memory = datapath::dedicated::DataMemory;
namespace common = colab::devices::common;
static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static constexpr unsigned VA = 0x40003000, PTE = 0x200c;
static unsigned leaf(unsigned pa, unsigned flags = 0xcf) { return (pa >> 12 << 10) | flags; }
static InterruptSystem state(PrivilegeMode mode = PrivilegeMode::Supervisor) {
    InterruptSystem s; s.write(0x3b0, 0x1fffffff); s.write(0x3a0, 0x1f);
    s.write(0x180, 0x80000001); s.write(0x300, unsigned(mode) << 11); s.mret(); return s;
}
static Memory memory(unsigned pte = leaf(0x5000)) {
    Memory m(16384); m.write(0x1400, 0x801); m.write(PTE, pte); return m;
}
static unsigned translate(Sv32& vm, InterruptSystem& s, Memory& m, Access access = Access::Read, unsigned address = VA) {
    auto pa = vm.translate(address, 4, access, 0x9000, s, nullptr, m);
    require(pa.has_value(), "local page walk did not finish"); return *pa;
}
static void timed_walk_one_pte_per_call() {
    struct ImmediateBus final : common::DmaBus {
        explicit ImmediateBus(Memory& memory) : memory(memory) {}
        Memory& memory;
        unsigned submitted = 0;
        std::optional<common::BusResult> result;
        common::Tag submit(common::Master, common::BusOperation operation, std::uint64_t address,
                           std::size_t bytes, common::Bytes = {}) override {
            require(operation == common::BusOperation::Read && bytes == 4 && !result, "unexpected timed walk transaction");
            ++submitted;
            result = common::BusResult{submitted, 0, colab::interconnect::pcie::Status::Success,
                                       common::word_bytes(memory.read(static_cast<std::uint32_t>(address))), 4, {}, false};
            return submitted;
        }
        bool ready(common::Tag tag) const override { return result && result->tag == tag; }
        common::BusResult take(common::Tag tag) override {
            require(ready(tag), "bad timed walk tag"); auto value = *result; result.reset(); return value;
        }
        bool is_memory(std::uint32_t, std::size_t) const override { return true; }
    };
    for (bool external : {false, true}) {
        auto s = state(); auto m = memory(); ImmediateBus bus(m);
        Sv32 vm({}, Sv32::WalkTiming::OnePtePerTick);
        auto first = vm.translate(VA, 4, Access::Read, 0x9000, s, external ? &bus : nullptr, m);
        require(!first && vm.busy() && vm.statistics().reads == 1 && (!external || bus.submitted == 1),
                "timed walk consumed more than one PTE on its first tick");
        auto second = vm.translate(VA, 4, Access::Read, 0x9000, s, external ? &bus : nullptr, m);
        require(second && *second == 0x5000 && !vm.busy() && vm.statistics().reads == 2 &&
                (!external || bus.submitted == 2), "timed walk did not finish on its second PTE tick");
        auto hit = vm.translate(VA, 4, Access::Read, 0x9000, s, external ? &bus : nullptr, m);
        require(hit && *hit == 0x5000 && vm.statistics().hits == 1 && (!external || bus.submitted == 2),
                "timed TLB hit incorrectly walked again");
    }
}
template<class Action> static void fault(Action action, RV32FaultCause cause, unsigned address = VA) {
    try { action(); } catch (const RV32Fault& f) {
        require(f.cause() == cause && f.pc() == 0x9000 && f.value() == address, "wrong fault cause/virtual PC/tval"); return;
    }
    throw std::runtime_error("missing Sv32 fault");
}
static void permissions() {
    auto s = state(); auto m = memory(); Sv32 vm;
    require(translate(vm, s, m, Access::Read, VA + 4) == 0x5004, "4K translation/offset failed");
    require(translate(vm, s, m, Access::Write) == 0x5000 && translate(vm, s, m, Access::Execute) == 0x5000,
            "RWX translation failed");
    require(vm.statistics().reads == 2 && vm.statistics().hits == 2, "TLB hit or real two-level walk count failed");
    // Recheck privileges and permissions on every cached translation.
    s.write(0x141, 0x9000); s.write(0x100, 0); s.sret();
    fault([&] { translate(vm, s, m); }, RV32FaultCause::LoadPageFault);
    for (auto access : {Access::Read, Access::Write, Access::Execute}) {
        auto u = state(PrivilegeMode::User); auto um = memory(leaf(0x5000, 0xdf)); Sv32 uv;
        require(translate(uv, u, um, access) == 0x5000, "user RWX mapping failed");
    }
    auto supervisor = state(); auto um = memory(leaf(0x5000, 0xdf)); Sv32 uv;
    fault([&] { translate(uv, supervisor, um); }, RV32FaultCause::LoadPageFault);
    supervisor.write(0x100, InterruptSystem::SUM);
    require(translate(uv, supervisor, um) == 0x5000, "SUM did not allow S data access to U page");
    fault([&] { translate(uv, supervisor, um, Access::Execute); }, RV32FaultCause::InstructionPageFault);
    auto xm = memory(leaf(0x5000, 0x49)); Sv32 xv; auto xs = state();
    fault([&] { translate(xv, xs, xm); }, RV32FaultCause::LoadPageFault);
    xs.write(0x100, InterruptSystem::MXR);
    require(translate(xv, xs, xm) == 0x5000, "MXR did not permit execute-only read");
    fault([&] { translate(xv, xs, xm, Access::Write); }, RV32FaultCause::StorePageFault);
    xs.write(0x100, 0); fault([&] { translate(xv, xs, xm); }, RV32FaultCause::LoadPageFault);
    for (unsigned flags : {0u, 5u, 0x0fu, 0x47u}) {
        auto bad = memory(leaf(0x5000, flags)); Sv32 bv; auto bs = state();
        fault([&] { translate(bv, bs, bad, flags == 0x47 ? Access::Write : Access::Read); },
            flags == 0x47 ? RV32FaultCause::StorePageFault : RV32FaultCause::LoadPageFault);
        require(!bv.busy(), "fault left walker busy");
    }
    // S-mode PMP is also applied to page-table reads and final physical accesses.
    auto ps = state(); auto pm = memory(); Sv32 pv;
    translate(pv, ps, pm); ps.write(0x3a0, 0x18); // Deny even after a TLB hit.
    fault([&] { translate(pv, ps, pm); }, RV32FaultCause::LoadAccessFault);
    pv.flush(); fault([&] { translate(pv, ps, pm, Access::Execute); }, RV32FaultCause::InstructionAccessFault);
    // M-mode ignores satp for instructions/data unless MPRV selects S/U for data.
    InterruptSystem ms; ms.write(0x180, 0x80000001); ms.write(0x3b0, 0x1fffffff); ms.write(0x3a0, 0x1f);
    Sv32 mv; require(translate(mv, ms, pm) == VA, "M-mode did not bypass Sv32");
    ms.write(0x300, InterruptSystem::MPRV | (1u << 11));
    require(translate(mv, ms, pm) == 0x5000 && translate(mv, ms, pm, Access::Execute) == VA, "MPRV translated fetch or failed data translation");
}
static void fault_matrix() {
    for (auto access : {Access::Read, Access::Write, Access::Execute}) {
        const auto page = access == Access::Read ? RV32FaultCause::LoadPageFault :
            access == Access::Write ? RV32FaultCause::StorePageFault : RV32FaultCause::InstructionPageFault;
        const auto denied = access == Access::Read ? RV32FaultCause::LoadAccessFault :
            access == Access::Write ? RV32FaultCause::StoreAccessFault : RV32FaultCause::InstructionAccessFault;
        for (unsigned flags : {0u, 5u, 0x0fu}) {
            auto s = state(); auto m = memory(leaf(0x5000, flags)); Sv32 vm;
            const auto original = m.read(PTE);
            fault([&] { translate(vm, s, m, access); }, page);
            require(m.read(PTE) == original && vm.statistics().page_faults == 1, "Svade fault modified PTE or wrong statistics");
        }
        for (unsigned reserved : {0x10u, 0x40u, 0x80u}) {
            auto s = state(); auto m = memory(); Sv32 vm; m.write(0x1400, 0x801 | reserved);
            fault([&] { translate(vm, s, m, access); }, page);
        }
        // RSW is ignored in both levels, unlike the non-leaf U/A/D bits.
        auto s = state(); auto m = memory(leaf(0x5000, 0x3cf)); Sv32 vm;
        m.write(0x1400, 0xb01); require(translate(vm, s, m, access) == 0x5000, "RSW affected translation");
        // Allow the page-table pages but not the final physical frame.
        s.write(0x3b0, 0x3000 / 4); s.write(0x3a0, 0x0b); vm.flush();
        fault([&] { translate(vm, s, m, access); }, denied);
        s.write(0x3a0, 0x08); vm.flush(); // Deny page-table reads too.
        fault([&] { translate(vm, s, m, access); }, denied);
        auto u = state(PrivilegeMode::User); auto sm = memory(); Sv32 uv;
        fault([&] { translate(uv, u, sm, access); }, page);
    }
    // Dirty is not required by fetch or load, but is required by store.
    auto s = state(); auto m = memory(leaf(0x5000, 0x4f)); Sv32 vm;
    require(translate(vm, s, m) == 0x5000 && translate(vm, s, m, Access::Execute) == 0x5000,
        "load/fetch incorrectly require D");
    fault([&] { translate(vm, s, m, Access::Write); }, RV32FaultCause::StorePageFault);
}
static void page_tables_and_tlb() {
    auto s = state(); auto m = memory(); Sv32 vm;
    m.write(0x1400, leaf(0x400000));
    require(translate(vm, s, m, Access::Read, VA + 0x1234) == 0x404234, "4M superpage offset failed");
    vm.flush(); m.write(0x1400, leaf(0x401000));
    fault([&] { translate(vm, s, m); }, RV32FaultCause::LoadPageFault);
    m = memory(); vm.flush(); m.write(0x1400, 0x811); // non-leaf U reserved
    fault([&] { translate(vm, s, m); }, RV32FaultCause::LoadPageFault);
    m = memory(); vm.flush(); m.write(PTE, 0x801); // no leaf at bottom level
    fault([&] { translate(vm, s, m); }, RV32FaultCause::LoadPageFault);
    m = memory(); vm.flush(); require(translate(vm, s, m) == 0x5000, "initial mapping failed");
    m.write(PTE, leaf(0x6000));
    require(translate(vm, s, m) == 0x5000, "TLB fixture did not retain old mapping before fence");
    vm.flush(); require(translate(vm, s, m) == 0x6000, "flush did not observe modified PTE");
    s.write(0x180, 0x80400001); m.write(PTE, leaf(0x7000));
    require(translate(vm, s, m) == 0x7000, "ASID reused another address space's non-global entry");
    // Global bit in non-leaf is inherited by its leaf.
    vm.flush(); m.write(0x1400, 0x821); require(translate(vm, s, m) == 0x7000, "global walk failed");
    s.write(0x180, 0x80800001); m.write(PTE, leaf(0x8000));
    require(translate(vm, s, m) == 0x7000, "non-leaf global bit was not inherited");
    vm.flush(); require(translate(vm, s, m) == 0x8000, "global entry not flushed");
    m.write(PTE, 0x400000cf); vm.flush(); // 34-bit leaf PPN outside this 32-bit platform.
    fault([&] { translate(vm, s, m); }, RV32FaultCause::LoadAccessFault);
    s.write(0x180, 0x80100000); vm.flush(); // High root PPN must not wrap into low RAM.
    fault([&] { translate(vm, s, m); }, RV32FaultCause::LoadAccessFault);
}
class SlowBus : public common::DmaBus {
public:
    Memory ram = memory(); bool pending = false, complete = false, error = false, short_read = false, ram_window = true;
    unsigned count = 0, delay = 0, address = 0;
    common::Tag submit(common::Master, common::BusOperation op, std::uint64_t at, std::size_t bytes, common::Bytes = {}) override {
        require(!pending && !complete && op == common::BusOperation::Read && bytes == 4, "overlapping/invalid page-table transaction");
        address = static_cast<unsigned>(at); pending = true; delay = 3; return ++count;
    }
    bool ready(common::Tag tag) const override { return complete && tag == count; }
    common::BusResult take(common::Tag tag) override {
        require(ready(tag), "premature take"); complete = false;
        common::BusResult result{}; result.tag = tag;
        result.status = error ? colab::interconnect::pcie::Status::Unmapped : colab::interconnect::pcie::Status::Success;
        result.transferred_bytes = short_read ? 0 : 4; result.data = common::word_bytes(ram.read(address)); return result;
    }
    bool is_memory(std::uint32_t, std::size_t) const override { return ram_window; }
    void tick() { if (pending && !--delay) { pending = false; complete = true; } }
};
static void async_walk() {
    auto s = state(); Memory local(1); Sv32 vm; SlowBus bus;
    auto step = [&] { return vm.translate(VA, 4, Access::Read, 0x9000, s, &bus, local); };
    require(!step() && bus.count == 1 && vm.busy(), "walk bypassed asynchronous bus");
    vm.cancel(); require(!step(), "redirect did not wait for abandoned read");
    for (unsigned i = 0; i < 3; ++i) bus.tick();
    require(!step() && bus.count == 2, "abandoned response was not drained before new request");
    std::optional<unsigned> result;
    for (unsigned i = 0; i < 20 && !result; ++i) { bus.tick(); result = step(); }
    require(result == 0x5000 && !vm.busy() && bus.count == 3, "slow walk lost/duplicated request or stayed busy");
    for (unsigned kind = 0; kind < 3; ++kind) {
        vm.flush(); bus.error = kind == 0; bus.short_read = kind == 1; bus.ram_window = kind != 2;
        bool failed = false;
        for (unsigned i = 0; i < 20 && !failed; ++i) {
            bus.tick(); try { step(); } catch (const RV32Fault& f) {
                require(f.cause() == RV32FaultCause::LoadAccessFault && f.value() == VA, "bus error was not a virtual-address access fault"); failed = true;
            }
        }
        require(failed && !vm.busy(), "bus failure left permanent walker busy state");
    }
    bus.error = false; bus.short_read = false; bus.ram_window = true;
    result.reset();
    for (unsigned i = 0; i < 20 && !result; ++i) { bus.tick(); result = step(); }
    require(result == 0x5000 && !vm.busy(), "legal access did not recover after page-table bus errors");
}
// Delayed byte-accurate port for CPU contract tests; SystemC/Cache/DIMM are
// exercised independently by systemc_compiled_paged, not emulated by this double.
class CoreBus : public common::DmaBus {
public:
    Memory ram{16384};
    std::optional<unsigned> fail_address;
    struct Request { unsigned address; common::BusOperation operation; };
    std::vector<Request> requests;
    common::Tag submit(common::Master, common::BusOperation op, std::uint64_t at, std::size_t bytes, common::Bytes payload = {}) override {
        require(!pending && !complete && at < 65536 && bytes == 4, "invalid CPU bus request");
        operation = op; address = at; requests.push_back({address, op}); data = std::move(payload); pending = true; delay = 2; return ++tag;
    }
    bool ready(common::Tag value) const override { return complete && value == tag; }
    common::BusResult take(common::Tag value) override {
        require(ready(value), "CPU read before ready"); complete = false;
        common::BusResult r{}; r.tag = tag; r.status = colab::interconnect::pcie::Status::Success; r.transferred_bytes = 4;
        if (fail_address == address) { r.status = colab::interconnect::pcie::Status::Unmapped; r.transferred_bytes = 0; return r; }
        if (operation == common::BusOperation::Read) r.data = common::word_bytes(ram.read(address));
        return r;
    }
    bool is_memory(std::uint32_t at, std::size_t bytes) const override { return std::uint64_t(at) + bytes <= 65536; }
    void tick() {
        if (pending && !--delay) {
            pending = false; complete = true;
            if (operation == common::BusOperation::Write && fail_address != address) ram.write(address, common::load32(data));
        }
    }
private:
    bool pending = false, complete = false;
    unsigned address = 0, delay = 0; common::Tag tag = 0;
    common::BusOperation operation{}; common::Bytes data;
};
static void cpu_paths() {
    using CPU = colab::soc::RV32Core;
    for (auto mode : {CPU::Mode::Hardwired, CPU::Mode::Microprogrammed}) for (bool readonly : {false, true}) {
        CoreBus bus;
        bus.ram.write(0x8000, 0x30200073); // MRET -> virtual S code.
        bus.ram.write(0x9000, 0x0000a503); bus.ram.write(0x9004, 0x00150513); // LW a0; ADDI a0,1.
        bus.ram.write(0x9008, 0x00a0a223); bus.ram.write(0x900c, 0x12000073); // SW a0,4; SFENCE.
        bus.ram.write(0x9010, 0x10500073); // WFI.
        bus.ram.write(0x1400, 0x801); bus.ram.write(0x2024, leaf(0x9000, 0x4b));
        bus.ram.write(0x2030, leaf(0xc000, readonly ? 0x43 : 0xc7)); bus.ram.write(0xc000, 41);
        CPU cpu({}, mode, 256, {}, CPU::Execution::MultiCycle, {}, FaultHandling::MachineTrap);
        cpu.connect_bus(bus); cpu.connect_instruction_bus(bus); cpu.set_boot_pc(0x8000); cpu.set_register(1, 0x4000c000);
        auto& s = cpu.interrupts(); s.write(0x3b0, 0x1fffffff); s.write(0x3a0, 0x1f);
        s.write(0x180, 0x80000001); s.write(0x341, 0x40009000); s.write(0x305, 0x8800); s.write(0x300, 1u << 11);
        for (unsigned i = 0; i < 2000 && !cpu.waiting() && !s.exceptions(); ++i) { bus.tick(); cpu.tick(); }
        require(cpu.registers().read(10) == 42 && cpu.bus_waits(), "multicycle paged load/ALU failed");
        if (readonly) require(s.exceptions() == 1 && s.read(0x342) == 15 && s.read(0x341) == 0x40009008 &&
            s.read(0x343) == 0x4000c004 && cpu.completed_instructions() == 3 && !bus.ram.read(0xc004),
            "faulting translated store retired or damaged memory/trap context");
        else require(cpu.waiting() && cpu.completed_instructions() == 6 && bus.ram.read(0xc004) == 42 &&
            cpu.sv32().statistics().flushes == 1, "paged store/SFENCE/WFI failed in multicycle control mode");
    }
}
static void architectural_switching() {
    using CPU = colab::soc::RV32Core;
    for (auto mode : {CPU::Mode::Hardwired, CPU::Mode::Microprogrammed}) {
        CoreBus bus; bus.ram.write(0x8000, 0x30200073);
        const unsigned code[]{0x0000a503, 0x18011073, 0x12000073, 0x0000a583,
            0x18019073, 0x12000073, 0x0000a603, 0x18001073, 0x0000a683, 0x10500073};
        for (unsigned i = 0; i < std::size(code); ++i) bus.ram.write(0x9000 + 4*i, code[i]);
        bus.ram.write(0x1000, 0x801); bus.ram.write(0x2000 + 9*4, leaf(0x9000, 0x4b));
        bus.ram.write(0x2000 + 6*4, leaf(0xc000, 0xc7));
        bus.ram.write(0x3000, 0x1001); bus.ram.write(0x4000 + 9*4, leaf(0x9000, 0x4b));
        bus.ram.write(0x4000 + 6*4, leaf(0xd000, 0xc7));
        bus.ram.write(0x6000, 11); bus.ram.write(0xc000, 22); bus.ram.write(0xd000, 33); bus.ram.write(0xe000, 99);
        CPU cpu({}, mode, 256, {}, CPU::Execution::MultiCycle, {}, FaultHandling::MachineTrap, CPU::Translation::Architectural);
        cpu.connect_bus(bus); cpu.connect_instruction_bus(bus); cpu.set_boot_pc(0x8000);
        cpu.set_register(1, 0x6000); cpu.set_register(2, 0x80000001); cpu.set_register(3, 0x80000003);
        auto& s = cpu.interrupts(); s.write(0x3b0, 0x1fffffff); s.write(0x3a0, 0x1f);
        s.write(0x341, 0x9000); s.write(0x300, 1u << 11);
        unsigned teaching_walks = 0;
        cpu.mmu().enable_paging(0, [&](auto entry_address) -> mmu::PageTableEntry {
            const auto vpn = entry_address / 8;
            ++teaching_walks; return {vpn == 6 ? 14u : vpn, true, {true,true,true,true},true,true,0};
        });
        for (unsigned i = 0; i < 3000 && !cpu.waiting() && !s.exceptions(); ++i) { bus.tick(); cpu.tick(); }
        require(cpu.waiting() && !s.exceptions() && !teaching_walks && cpu.registers().read(10) == 11 &&
            cpu.registers().read(11) == 22 && cpu.registers().read(12) == 33 && cpu.registers().read(13) == 11 &&
            s.read(0x180) == 0 && cpu.sv32().statistics().flushes == 2,
            "Bare/Sv32 root switch mixed teaching MMU or satp implicitly flushed");
        // The public legacy default still intentionally supports host-configured teaching translations.
        CoreBus legacy_bus; legacy_bus.ram.write(0x8000, 0x0000a503); legacy_bus.ram.write(0xe000, 99);
        CPU legacy({}, mode); legacy.connect_bus(legacy_bus); legacy.connect_instruction_bus(legacy_bus);
        legacy.set_boot_pc(0x8000); legacy.set_register(1, 0x6000);
        legacy.mmu().enable_paging(0, [](auto entry_address) -> mmu::PageTableEntry {
            const auto vpn = entry_address / 8;
            return {vpn == 6 ? 14u : vpn, true, {true,true,true,true},true,true,0};
        });
        for (unsigned i = 0; i < 1000 && !legacy.completed_instructions(); ++i) { legacy_bus.tick(); legacy.tick(); }
        require(legacy.registers().read(10) == 99, "architectural isolation broke existing teaching-MMU API");
    }
}
static void cpu_fault_contract() {
    using CPU = colab::soc::RV32Core;
    for (auto mode : {CPU::Mode::Hardwired, CPU::Mode::Microprogrammed})
    for (auto access : {Access::Read, Access::Write, Access::Execute}) for (unsigned kind = 0; kind < 5; ++kind)
    for (bool delegated : {false, true}) {
        CoreBus bus; bus.ram.write(0x8000, 0x30200073);
        bus.ram.write(0x9000, access == Access::Write ? 0x00a0a023 : 0x0000a503);
        bus.ram.write(0x1400, 0x801); bus.ram.write(0x2024, leaf(0x9000, 0x4b));
        bus.ram.write(0x1500, 0xc01); bus.ram.write(0x3030, leaf(0xc000, 0xc7)); bus.ram.write(0xc000, 41);
        CPU cpu({}, mode, 256, {}, CPU::Execution::MultiCycle, {}, FaultHandling::MachineTrap, CPU::Translation::Architectural);
        cpu.connect_bus(bus); cpu.connect_instruction_bus(bus); cpu.set_boot_pc(0x8000);
        cpu.set_register(1, 0x5000c000); cpu.set_register(10, 77);
        const unsigned pte = access == Access::Execute ? 0x2024 : 0x3030;
        const unsigned physical = access == Access::Execute ? 0x9000 : 0xc000;
        const unsigned va = access == Access::Execute ? 0x40009000 : 0x5000c000;
        const unsigned cause = kind == 0 ? (access == Access::Read ? 13 : access == Access::Write ? 15 : 12) :
            (access == Access::Read ? 5 : access == Access::Write ? 7 : 1);
        auto& s = cpu.interrupts(); s.write(0x3b0, 0x1fffffff); s.write(0x3a0, 0x1f);
        s.write(0x180, 0x80000001); s.write(0x341, 0x40009000); s.write(0x305, 0x8800);
        s.write(0x105, 0x40008801); s.write(0x300, 1u << 11); s.write(0x302, delegated ? 1u << cause : 0);
        if (kind == 0) bus.ram.write(pte, 0); // Invalid PTE -> page fault, not access fault.
        if (kind == 2 || kind == 4) bus.fail_address = kind == 2 ? pte : physical;
        if (kind == 1 || kind == 3) { // Deny walk page or final PA only; allow the other stages.
            const auto denied = kind == 1 ? pte & ~0xfffu : physical;
            s.write(0x3b0, denied / 4); s.write(0x3b1, (denied + 0x1000) / 4);
            s.write(0x3b2, 0x3fffffff); s.write(0x3a0, 0x0f080f);
        }
        for (unsigned i = 0; i < 3000 && !s.exceptions(); ++i) { bus.tick(); cpu.tick(); }
        const auto bank = delegated ? 0x140u : 0x340u;
        require(s.exceptions() == 1 && s.read(bank+2) == cause && s.read(bank+1) == 0x40009000 && s.read(bank+3) == va &&
            cpu.pc() == (delegated ? 0x40008800 : 0x8800) && cpu.completed_instructions() == 1 &&
            cpu.registers().read(10) == 77 && bus.ram.read(0xc000) == 41 &&
            s.privilege() == (delegated ? PrivilegeMode::Supervisor : PrivilegeMode::Machine),
            "I/L/S page/access fault delegation lost precise PC/value, privilege, retirement or side-effect boundary");
        const auto final_requests = std::count_if(bus.requests.begin(), bus.requests.end(),
            [&](auto request) { return request.address == physical; });
        require(final_requests == (kind == 4 ? 1 : 0), "denied translation issued a final physical request");
    }
    for (auto mode : {CPU::Mode::Hardwired, CPU::Mode::Microprogrammed})
    for (auto instruction : {0x00009503u, 0x0000a503u, 0x00a09023u, 0x00a0a023u, 0x000080e7u}) {
        CoreBus bus; bus.ram.write(0x8000, 0x30200073); bus.ram.write(0x9000, instruction);
        bus.ram.write(0x1400, 0x801); bus.ram.write(0x2024, leaf(0x9000, 0x4b));
        const bool jump = instruction == 0x000080e7u, store = (instruction & 0x7f) == 0x23;
        const unsigned target = jump ? 0x5000c002 : 0x5000c001;
        CPU cpu({}, mode, 256, {}, CPU::Execution::MultiCycle, {}, FaultHandling::MachineTrap, CPU::Translation::Architectural);
        cpu.connect_bus(bus); cpu.connect_instruction_bus(bus); cpu.set_boot_pc(0x8000); cpu.set_register(1, target);
        auto& s = cpu.interrupts(); s.write(0x3b0, 0x1fffffff); s.write(0x3a0, 0x1f); s.write(0x180, 0x80000001);
        s.write(0x341, 0x40009000); s.write(0x305, 0x8800); s.write(0x300, 1u << 11);
        for (unsigned i = 0; i < 2000 && !s.exceptions(); ++i) { bus.tick(); cpu.tick(); }
        require(s.read(0x342) == (jump ? 0 : store ? 6 : 4) && s.read(0x343) == target && s.read(0x341) == 0x40009000 &&
            cpu.completed_instructions() == 1 && cpu.sv32().statistics().walks == 1 && cpu.sv32().statistics().reads == 2 &&
            cpu.registers().read(1) == target, "alignment-first contract walked a bad target or committed a faulting JALR");
    }
}
static void mprv_matrix() {
    for (auto mpp : {PrivilegeMode::User, PrivilegeMode::Supervisor, PrivilegeMode::Machine})
    for (bool user : {false, true}) for (bool sum : {false, true}) for (bool deny : {false, true}) {
        InterruptSystem s; s.write(0x180, 0x80000001); s.write(0x3b0, 0x1fffffff); s.write(0x3a0, deny ? 0x18 : 0x1f);
        s.write(0x300, InterruptSystem::MPRV | (unsigned(mpp)<<11) | (sum ? InterruptSystem::SUM : 0));
        auto m = memory(leaf(0x5000, 0xcf | (user ? 16 : 0))); Sv32 vm;
        require(translate(vm, s, m, Access::Execute) == VA && vm.statistics().reads == 0, "MPRV incorrectly translated M fetch");
        for (auto access : {Access::Read, Access::Write}) {
            if (mpp == PrivilegeMode::Machine) require(translate(vm, s, m, access) == VA, "MPRV MPP=M translated data");
            else if (deny) fault([&] { translate(vm, s, m, access); },
                access == Access::Read ? RV32FaultCause::LoadAccessFault : RV32FaultCause::StoreAccessFault);
            else if (mpp == PrivilegeMode::User ? !user : user && !sum)
                fault([&] { translate(vm, s, m, access); }, access == Access::Read ? RV32FaultCause::LoadPageFault : RV32FaultCause::StorePageFault);
            else require(translate(vm, s, m, access) == 0x5000, "MPRV/SUM/PTE combination failed");
        }
    }
}
int main() {
    timed_walk_one_pte_per_call(); permissions(); fault_matrix(); page_tables_and_tlb(); async_walk(); cpu_paths();
    architectural_switching(); cpu_fault_contract(); mprv_matrix();
    std::cout << "PASS Sv32: 4K/4M, U/S, SUM/MXR/MPRV, A/D faults, PMP, ASIDs/global/fence, 34-bit rejection, delayed/cancelled/error walks\n";
}
