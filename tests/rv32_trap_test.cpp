#include "../soc/rv32/core.hpp"
#include <iostream>

namespace c = colab::devices::common;
using CPU = colab::soc::RV32Core;
using Mode = CPU::Mode;
using Cause = colab::soc::core::RV32FaultCause;
using Fault = colab::soc::core::RV32Fault;
using Handling = colab::soc::core::FaultHandling;
using colab::soc::core::RV32Trap;
static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static std::uint32_t i(unsigned rd, unsigned source, int immediate, bool load = false) {
    return ((std::uint32_t(immediate) & 4095) << 20) | (source << 15) | (rd << 7) | (load ? 0x2003 : 0x13);
}
static std::uint32_t sw(unsigned source, unsigned base, int offset) {
    const auto imm = std::uint32_t(offset) & 4095;
    return ((imm >> 5) << 25) | (source << 20) | (base << 15) | ((imm & 31) << 7) | 0x2023;
}
static std::uint32_t beq(unsigned a, unsigned b, unsigned offset) {
    return (((offset >> 12) & 1) << 31) | (((offset >> 5) & 63) << 25) | (b << 20) |
        (a << 15) | (((offset >> 1) & 15) << 8) | (((offset >> 11) & 1) << 7) | 0x63;
}
static std::uint32_t csr(unsigned number, unsigned rd, unsigned source, bool read = false) {
    return (number << 20) | (source << 15) | (read ? 0x2000u : 0x1000u) | (rd << 7) | 0x73;
}
static CPU make_cpu(std::vector<std::uint32_t> program, unsigned config, Handling handling = Handling::MachineTrap) {
    return CPU(std::move(program), config == 1 ? Mode::Microprogrammed : Mode::Hardwired, 64, {},
        config >= 2 ? CPU::Execution::Pipelined : CPU::Execution::MultiCycle, {config != 3}, handling);
}
class Bus : public c::DmaBus {
public:
    struct Request { c::BusOperation op; std::uint64_t address; std::size_t count; c::Bytes data; };
    c::Bytes memory = c::Bytes(256);
    std::vector<Request> requests;
    c::Tag submit(c::Master, c::BusOperation op, std::uint64_t address, std::size_t count, c::Bytes data = {}) override {
        require(!pending && !response, "duplicate in-flight request");
        requests.push_back({op, address, count, std::move(data)}); pending = true; delay = 4;
        return requests.size();
    }
    bool ready(c::Tag tag) const override { return response && response->tag == tag; }
    c::BusResult take(c::Tag tag) override {
        require(ready(tag), "trap consumed unfinished transaction");
        auto result = *response; response.reset(); return result;
    }
    void tick() {
        if (!pending || --delay) return;
        const auto& request = requests.back();
        c::BusResult result{}; result.tag = requests.size();
        result.status = colab::interconnect::pcie::Status::BackendError;
        if (request.address <= memory.size() && request.count <= memory.size() - request.address) {
            result.status = colab::interconnect::pcie::Status::Success;
            result.transferred_bytes = request.count;
            if (request.op == c::BusOperation::Read)
                result.data.assign(memory.begin() + request.address, memory.begin() + request.address + request.count);
            else std::copy(request.data.begin(), request.data.end(), memory.begin() + request.address);
        }
        pending = false; response = result;
    }
private:
    bool pending = false;
    unsigned delay = 0;
    std::optional<c::BusResult> response;
};
static RV32Trap trap(CPU& cpu, Bus* bus = nullptr) {
    for (unsigned n = 0; n < 1000; ++n) {
        const auto before = cpu.clock_pulses();
        if (bus) bus->tick();
        cpu.tick();
        require(cpu.clock_pulses() == before + 1, "trap lost or duplicated a clock pulse");
        if (cpu.last_cycle()->trap) {
            require(!cpu.last_cycle()->instruction_complete && !cpu.last_cycle()->interrupt_entry,
                "exception was retired or counted as IRQ");
            return *cpu.last_cycle()->trap;
        }
    }
    throw std::runtime_error("machine trap not delivered");
}
static std::vector<std::uint32_t> image(std::uint32_t fault) {
    std::vector<std::uint32_t> program(64, beq(0, 0, 0));
    program[0] = i(8, 0, 19); program[1] = fault; program[2] = i(4, 0, 7);
    program[32] = csr(0x342, 10, 0, true);
    program[33] = csr(0x341, 11, 0, true);
    program[34] = csr(0x343, 12, 0, true);
    program[35] = csr(0x300, 13, 0, true);
    program[36] = i(11, 11, 4);
    program[37] = csr(0x341, 0, 11);
    program[38] = 0x30200073;
    return program;
}
static void delivery_and_return() {
    struct Case { std::uint32_t instruction, address; Cause cause; std::uint32_t value; };
    const std::vector<Case> cases{
        {0x00000073, 0, Cause::EnvironmentCallFromM, 0},
        {0x00100073, 0, Cause::Breakpoint, 0},
        {0xffffffff, 0, Cause::IllegalInstruction, 0xffffffff},
        {csr(0xfff, 3, 0), 0, Cause::IllegalInstruction, csr(0xfff, 3, 0)},
        {i(3, 1, 0, true), 2, Cause::LoadAddressMisaligned, 2},
        {i(0, 1, 0, true), 256, Cause::LoadAccessFault, 256},
        {sw(2, 1, 0), 2, Cause::StoreAddressMisaligned, 2},
        {sw(2, 1, 0), 256, Cause::StoreAccessFault, 256},
        {beq(0, 0, 2), 0, Cause::InstructionAddressMisaligned, 6},
        {0x002001ef, 0, Cause::InstructionAddressMisaligned, 6},
    };
    for (unsigned config = 0; config < 4; ++config) for (bool external : {false, true})
        for (bool enabled : {false, true}) for (const auto& item : cases) {
            auto cpu = make_cpu(image(item.instruction), config);
            Bus bus;
            cpu.set_register(1, item.address); cpu.set_register(2, 0x12345678);
            cpu.set_register(3, 99); cpu.set_register(4, 88); cpu.set_memory(0, 0xaabbccdd);
            if (external) cpu.connect_bus(bus);
            cpu.interrupts().write(0x305, 129); // Synchronous traps still enter BASE in Vectored mode.
            cpu.interrupts().write(0x300, enabled ? 8 : 0);
            const auto result = trap(cpu, external ? &bus : nullptr);
            require(result.cause == item.cause && result.pc == 4 && result.value == item.value && result.target == 128,
                "incorrect exception trace/vector");
            require(cpu.pc() == 128 && cpu.completed_instructions() == 1 && cpu.registers().read(8) == 19 &&
                cpu.registers().read(3) == 99 && cpu.registers().read(4) == 88, "trap was not precise");
            require(cpu.interrupts().exceptions() == 1 && cpu.interrupts().entries() == 0 &&
                cpu.interrupts().read(0x341) == 4 && cpu.interrupts().read(0x342) == unsigned(item.cause) &&
                cpu.interrupts().read(0x343) == item.value, "exception CSRs differed");
            require((cpu.interrupts().read(0x300) & 0x88u) == (enabled ? 0x80u : 0u), "trap MIE/MPIE stack differed");
            require(cpu.memory().read(0) == 0xaabbccdd && bus.memory == c::Bytes(256), "faulting store changed memory");
            for (unsigned n = 0; n < 1000 && cpu.registers().read(4) != 7; ++n) { if (external) bus.tick(); cpu.tick(); }
            require(cpu.interrupts().returns() == 1 && cpu.registers().read(4) == 7 && cpu.pc() == 12 &&
                cpu.completed_instructions() == 9 && cpu.registers().read(10) == unsigned(item.cause) &&
                cpu.registers().read(12) == item.value, "guest trap handler failed to resume");
            require((cpu.interrupts().read(0x300) & 0x88u) == (enabled ? 0x88u : 0x80u), "MRET did not restore interrupt state");
        }
}
static void precision_and_translation() {
    for (unsigned config = 0; config < 4; ++config) {
        auto zero_vector = make_cpu({0x00000073}, config);
        require(trap(zero_vector).target == 0 && zero_vector.interrupts().exceptions() == 1 &&
            !zero_vector.completed_instructions(), "mtvec=0 incorrectly disabled synchronous traps");
        // Younger destructive reads and stores must not escape before an older fault.
        for (auto younger : {i(5, 1, 0, true), sw(2, 1, 0)}) {
            auto program = image(0xffffffff); program[2] = younger;
            auto cpu = make_cpu(program, config); Bus bus;
            cpu.set_register(1, 32); cpu.connect_bus(bus); cpu.interrupts().write(0x305, 128);
            trap(cpu, &bus);
            require(bus.requests.empty(), "younger memory side effect escaped trap squash");
        }
        // Trap handler faults again: no fictitious retirement and no enable-bit requirement.
        auto nested_program = image(0x00000073); nested_program[32] = 0x00100073;
        auto nested = make_cpu(nested_program, config); nested.interrupts().write(0x305, 128);
        trap(nested); const auto second = trap(nested);
        require(second.cause == Cause::Breakpoint && second.pc == 128 && nested.completed_instructions() == 1 &&
            nested.interrupts().exceptions() == 2, "nested synchronous trap was lost");

        auto fetch_program = image(0x00008067); // JALR x0,x1,0 into unmapped ROM.
        auto fetch = make_cpu(fetch_program, config); fetch.set_register(1, 0x400);
        fetch.interrupts().write(0x305, 128);
        const auto fetch_fault = trap(fetch);
        require(fetch_fault.cause == Cause::InstructionAccessFault && fetch_fault.pc == 0x400 &&
            fetch_fault.value == 0x400 && fetch.completed_instructions() == 2, "fetch fault lost original PC");

        namespace mmu = colab::soc::core::mmu;
        if (config < 2) {
            auto instruction_page = make_cpu({}, config); Bus instructions;
            instruction_page.set_boot_pc(4); instruction_page.connect_instruction_bus(instructions);
            instruction_page.interrupts().write(0x305, 128);
            instruction_page.mmu().enable_paging(0, [](auto) -> mmu::PageTableEntry {
                return {0, true, {true, true, false, false}, true, false, 0};
            });
            const auto failure = trap(instruction_page, &instructions);
            require(failure.cause == Cause::InstructionPageFault && failure.pc == 4 && failure.value == 4 &&
                instructions.requests.empty(), "instruction page fault escaped to bus");
        }
        for (bool store_access : {false, true}) for (bool broken_table : {false, true}) {
            auto cpu = make_cpu(image(store_access ? sw(2, 1, 0) : i(3, 1, 0, true)), config); Bus bus;
            cpu.set_register(1, 0x2000); cpu.connect_bus(bus); cpu.interrupts().write(0x305, 128);
            cpu.mmu().enable_paging(0, [broken_table](auto) -> mmu::PageTableEntry {
                if (broken_table) throw std::out_of_range("missing page table RAM");
                return {0, true, {false, false, true, false}, true, false, 0};
            });
            const auto failure = trap(cpu, &bus);
            const auto expected = broken_table ? (store_access ? Cause::StoreAccessFault : Cause::LoadAccessFault)
                : (store_access ? Cause::StorePageFault : Cause::LoadPageFault);
            require(failure.cause == expected && failure.pc == 4 && failure.value == 0x2000 &&
                bus.requests.empty(), "translation fault issued data request or lost cause/address");
        }
    }
    // Explicit host diagnostics remain available to standalone teaching harnesses.
    for (auto word : {0x00000073u, 0x00100073u}) {
        auto cpu = make_cpu({word}, 0, Handling::HostException);
        bool caught = false;
        try { cpu.step_instruction(); }
        catch (const Fault& fault) { caught = fault.cause() == (word == 0x73 ? Cause::EnvironmentCallFromM : Cause::Breakpoint); }
        require(caught && !cpu.completed_instructions() && !cpu.interrupts().exceptions(), "host fault policy changed");
    }
}
static void csr_state() {
    colab::soc::core::InterruptSystem state;
    state.write(0x340, 0x12345678); state.write(0x343, 0xffffffff); state.write(0x305, 129);
    require(state.read(0x340) == 0x12345678 && state.read(0x343) == 0xffffffff, "scratch/trap value CSR failed");
    require(state.exception(4, 2, 0xdeadbeef) == 128 && state.read(0x343) == 0xdeadbeef, "exception vector failed");
    state.write(0x300, 8); state.write(0x304, 1u << 11); state.set_external(true);
    require(state.enter(8) == 172 && state.read(0x343) == 0 && state.read(0x342) == 0x8000000b,
        "IRQ vector or clearing mtval failed");
}
int main() {
    delivery_and_return(); precision_and_translation(); csr_state();
    std::cout << "PASS RV32 machine traps: precise ECALL/EBREAK/faults, CSR state, MRET, four CPU configurations and delayed bus\n";
}
