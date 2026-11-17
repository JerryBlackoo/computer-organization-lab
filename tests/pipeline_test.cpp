#include "rv32_reference.hpp"
#include "../soc/soc.hpp"
#include "../soc/core/mmu/page_table_port/page_table_port.hpp"

#include <iostream>
#include <map>
#include <random>

using namespace rv32_test;
namespace p = colab::soc::core::pipeline;
namespace c = colab::devices::common;
using Execution = CPU::Execution;

static CPU pipeline(std::vector<std::uint32_t> program, bool forwarding = true) {
    return CPU(std::move(program), Mode::Hardwired, 64, {}, Execution::Pipelined, {forwarding});
}
static std::uint32_t csr(unsigned address, unsigned rd, unsigned rs1, bool set = false) {
    return (address << 20) | (rs1 << 15) | ((set ? 2u : 1u) << 12) | (rd << 7) | 0x73u;
}

static void timing_and_hazards() {
    auto cpu = pipeline({i(1, 0, 10), i(2, 0, 20), i(3, 0, 30), i(4, 0, 40), i(5, 0, 50)});
    for (unsigned n = 1; n <= 9; ++n) {
        cpu.tick();
        assert(cpu.clock_pulses() == n);
        assert(cpu.completed_instructions() == (n < 5 ? 0 : n - 4));
        if (n == 5) {
            const auto& trace = *cpu.last_pipeline_cycle();
            for (unsigned stage = 0; stage < 5; ++stage) assert(trace.pc[stage] == 16u - stage * 4);
        }
    }
    assert(cpu.registers().read(5) == 50 && cpu.pc() == 20);
    assert(cpu.last_cycle()->execution == Execution::Pipelined);
    expect_throw<std::invalid_argument>([&] { cpu.select_mode(Mode::Microprogrammed); });
    expect_throw<std::invalid_argument>([] { CPU invalid({}, Mode::Microprogrammed, 64, {}, Execution::Pipelined); });

    for (bool forwarding : {false, true}) {
        auto chain = pipeline({i(1, 0, 10), i(1, 1, 1), r(2, 1, 1), sw(2, 0, 0),
            i(3, 0, 0, true), r(4, 3, 2), beq(4, 4, 8), sw(1, 0, 4), i(5, 4, -1)}, forwarding);
        unsigned stalls = 0;
        while (chain.completed_instructions() < 8) {
            chain.tick();
            stalls += chain.last_pipeline_cycle()->data_stall;
            assert(chain.clock_pulses() < 100);
        }
        assert(chain.registers().read(1) == 11 && chain.registers().read(2) == 22);
        assert(chain.registers().read(3) == 22 && chain.registers().read(4) == 44 && chain.registers().read(5) == 43);
        assert(chain.memory().read(0) == 22 && chain.memory().read(4) == 0);
        assert(stalls == (forwarding ? 1u : 10u));
    }
    // x0 is not a producer; immediate bit fields do not create false rs2 dependencies.
    auto zero = pipeline({i(0, 0, 0, true), i(1, 0, 0), i(2, 0, 1)});
    zero.set_memory(0, 100);
    zero.run_instructions(3);
    assert(zero.registers().read(0) == 0 && zero.clock_pulses() == 7);
    // A wraparound address and signed overflow retain RV32 semantics.
    auto wrap = pipeline({i(1, 1, 1), sw(1, 2, 4), i(3, 2, 4, true), r(4, 3, 1, true)});
    wrap.set_register(1, 0x7FFFFFFF); wrap.set_register(2, 0xFFFFFFFC);
    wrap.run_instructions(4);
    assert(wrap.registers().read(1) == 0x80000000 && wrap.registers().read(3) == 0x80000000);
    assert(wrap.registers().read(4) == 0);
}

// A clocked destructive-read bus: tests can see every accepted request and effect.
struct Bus : c::DmaBus {
    struct Request { c::Tag tag; c::BusOperation op; std::uint64_t address; c::Bytes data; unsigned remaining; };
    std::optional<Request> pending;
    std::optional<c::BusResult> result;
    std::map<std::uint64_t, std::uint32_t> words;
    std::vector<std::uint64_t> addresses;
    unsigned delay = 5, reject = 0, reads = 0, writes = 0;
    bool fail = false, short_read = false;
    c::Tag next = 1;
    c::Tag submit(c::Master master, c::BusOperation op, std::uint64_t address,
                  std::size_t count, c::Bytes data = {}) override {
        assert(master == 0 && count == 4);
        if (reject) { --reject; throw std::length_error("backpressure"); }
        assert(!pending && !result);
        const auto tag = next++;
        pending = Request{tag, op, address, std::move(data), delay};
        addresses.push_back(address);
        return tag;
    }
    void tick() {
        if (!pending || result || --pending->remaining) return;
        c::BusResult response{};
        response.tag = pending->tag;
        response.status = fail ? colab::interconnect::pcie::Status::BackendError : colab::interconnect::pcie::Status::Success;
        response.transferred_bytes = fail ? 0 : 4;
        if (!fail) {
            if (pending->op == c::BusOperation::Write) { words[pending->address] = c::load32(pending->data); ++writes; }
            else {
                response.data = c::word_bytes(words[pending->address]); ++reads;
                if (short_read) response.data.resize(3);
            }
        }
        result = response;
    }
    bool ready(c::Tag tag) const override { return result && result->tag == tag; }
    c::BusResult take(c::Tag tag) override {
        assert(ready(tag)); auto response = *result; result.reset(); pending.reset(); return response;
    }
};
static void retire_bus(CPU& cpu, Bus& bus, std::uint64_t count) {
    const auto target = cpu.completed_instructions() + count;
    for (unsigned n = 0; cpu.completed_instructions() < target && n < 1000; ++n) { bus.tick(); cpu.tick(); }
    assert(cpu.completed_instructions() == target);
}

static void waits_and_side_effects() {
    Bus bus; bus.words[0] = 7; bus.reject = 3;
    // WB's x1 producer disappears while MEM holds the dependent EX instruction.
    auto cpu = pipeline({i(1, 0, 10), i(2, 0, 0, true), r(3, 1, 1), r(4, 2, 3),
        sw(4, 0, 4), i(5, 0, 4, true)});
    cpu.connect_bus(bus);
    retire_bus(cpu, bus, 6);
    assert(cpu.registers().read(3) == 20 && cpu.registers().read(4) == 27 && cpu.registers().read(5) == 27);
    assert(bus.reads == 2 && bus.writes == 1 && bus.words[4] == 27 && cpu.bus_waits() >= 3);
    assert(bus.addresses == std::vector<std::uint64_t>({0, 4, 4}));

    // A delayed older load can hold a younger M instruction while its WB
    // producer disappears. Both operands must retain the forwarded value.
    for (bool forwarding : {false, true}) {
        Bus delayed; delayed.words[0] = 7; delayed.reject = 3;
        const auto mul = r(3, 1, 1) | 0x02000000u;
        const auto div = r(4, 3, 2) | 0x02004000u;
        auto arithmetic = pipeline({i(1, 0, 10), i(2, 0, 0, true), mul, div}, forwarding);
        arithmetic.connect_bus(delayed);
        retire_bus(arithmetic, delayed, 4);
        assert(arithmetic.registers().read(3) == 100 && arithmetic.registers().read(4) == 14);
        assert(delayed.reads == 1 && arithmetic.bus_waits() >= 3);
    }

    // IRQ while an M operation is running behind a just-completed external
    // load must drain that irreversible read, squash M and restart at its PC.
    for (bool forwarding : {false, true}) {
        Bus delayed; delayed.words[0] = 7;
        std::vector<std::uint32_t> program(32, i(0, 0, 0));
        program[0] = i(2, 0, 0, true); program[1] = r(3, 1, 1) | 0x02000000u;
        program[2] = i(4, 3, 1); program[16] = 0x30200073u;
        auto irq = pipeline(program, forwarding); irq.set_register(1, 3); irq.set_register(3, 99);
        irq.connect_bus(delayed);
        irq.interrupts().write(0x305, 64); irq.interrupts().write(0x304, 1u << 11); irq.interrupts().write(0x300, 8);
        for (unsigned n = 0; n < 100 && !irq.pipeline().muldiv().busy(); ++n) { delayed.tick(); irq.tick(); }
        assert(irq.pipeline().muldiv().busy() && irq.pipeline().mem_wb().external_effect);
        irq.interrupts().set_external(true); delayed.tick(); irq.tick();
        assert(!irq.pipeline().muldiv().busy() && irq.registers().read(2) == 7 &&
               irq.registers().read(3) == 99 && irq.interrupts().entries() == 0 && delayed.reads == 1);
        delayed.tick(); irq.tick();
        assert(irq.interrupts().entries() == 1 && irq.interrupts().read(0x341) == 4);
        irq.interrupts().set_external(false);
        retire_bus(irq, delayed, 3); // MRET, retried MUL, dependent ADDI.
        assert(irq.registers().read(3) == 9 && irq.registers().read(4) == 10 && delayed.reads == 1);
    }

    Bus wrong_path;
    auto branch = pipeline({beq(0, 0, 12), sw(2, 1, 0), i(3, 1, 0, true), i(4, 0, 9)});
    branch.set_register(1, 0x10000000); branch.connect_bus(wrong_path);
    retire_bus(branch, wrong_path, 2);
    assert(wrong_path.addresses.empty() && branch.registers().read(4) == 9);
    // A fault must retire before any younger destructive read or store is submitted.
    for (auto younger : {sw(2, 1, 0), i(3, 1, 0, true)}) {
        Bus backend;
        auto fault = pipeline({i(4, 0, 3), 0xFFFFFFFFu, younger});
        fault.set_register(1, 0x10000000); fault.connect_bus(backend);
        bool caught = false;
        try { retire_bus(fault, backend, 3); }
        catch (const Fault& error) { caught = true; assert(error.cause() == Cause::IllegalInstruction && error.pc() == 4); }
        assert(caught && fault.completed_instructions() == 1 && fault.pc() == 4 && backend.addresses.empty());
    }
    // A load to x0 still performs its destructive MMIO read.
    Bus discard;
    auto x0 = pipeline({i(0, 0, 0, true)}); x0.connect_bus(discard);
    retire_bus(x0, discard, 1);
    assert(discard.reads == 1 && x0.registers().read(0) == 0);
    for (bool short_read : {false, true}) {
        Bus broken; broken.fail = !short_read; broken.short_read = short_read;
        auto fault = pipeline({i(3, 0, 0, true), sw(2, 0, 4)}); fault.connect_bus(broken);
        bool caught = false;
        try { retire_bus(fault, broken, 2); }
        catch (const Fault& error) { caught = true; assert(error.cause() == Cause::LoadAccessFault && error.pc() == 0); }
        assert(caught && fault.pc() == 0 && fault.completed_instructions() == 0 && broken.writes == 0);
    }
}

static void faults_and_csr() {
    struct Case { std::vector<std::uint32_t> program; std::uint32_t base; Cause cause; std::uint32_t pc, value; };
    const std::vector<Case> cases{
        {{}, 0, Cause::InstructionAccessFault, 0, 0},
        {{0}, 0, Cause::IllegalInstruction, 0, 0},
        {{i(3, 1, 0, true)}, 2, Cause::LoadAddressMisaligned, 0, 2},
        {{i(0, 1, 0, true)}, 2, Cause::LoadAddressMisaligned, 0, 2},
        {{i(3, 1, 0, true)}, 256, Cause::LoadAccessFault, 0, 256},
        {{sw(2, 1, 0)}, 2, Cause::StoreAddressMisaligned, 0, 2},
        {{sw(2, 1, 0)}, 0xFFFFFFFC, Cause::StoreAccessFault, 0, 0xFFFFFFFC},
        {{beq(0, 0, 2)}, 0, Cause::InstructionAddressMisaligned, 0, 2},
        {{beq(0, 0, -4)}, 0, Cause::InstructionAccessFault, 0xFFFFFFFC, 0xFFFFFFFC},
        {{csr(0xFFF, 3, 0)}, 0, Cause::IllegalInstruction, 0, csr(0xFFF, 3, 0)}
    };
    for (const auto& item : cases) {
        auto cpu = pipeline(item.program); cpu.set_register(1, item.base); cpu.set_register(3, 123);
        for (unsigned retry = 0; retry < 2; ++retry) {
            bool caught = false;
            try { for (unsigned n = 0; n < 32; ++n) cpu.tick(); }
            catch (const Fault& error) {
                caught = true; assert(error.cause() == item.cause && error.pc() == item.pc && error.value() == item.value);
            }
            assert(caught && cpu.registers().read(3) == 123 && cpu.pc() == item.pc);
        }
    }
    auto not_taken = pipeline({beq(1, 2, 2)});
    not_taken.set_register(1, 1); not_taken.set_register(2, 2); not_taken.step_instruction();
    assert(not_taken.pc() == 4);
    // Wrong-path illegal instruction, misaligned data access and CSR write disappear.
    auto flushed = pipeline({beq(0, 0, 8), 0xFFFFFFFF, beq(0, 0, 8), i(3, 0, 2, true),
        beq(0, 0, 8), csr(0x305, 0, 1), i(4, 0, 7)});
    flushed.set_register(1, 64); flushed.run_instructions(4);
    assert(flushed.interrupts().read(0x305) == 0 && flushed.registers().read(4) == 7);

    auto system = pipeline({i(1, 0, 64), csr(0x305, 2, 1), csr(0x305, 3, 0, true), i(4, 3, 1)});
    system.run_instructions(4);
    assert(system.registers().read(2) == 0 && system.registers().read(3) == 64 && system.registers().read(4) == 65);
    assert(system.interrupts().read(0x305) == 64);
}

static void interrupt_boundaries() {
    for (bool store : {false, true}) for (bool deassert : {false, true}) {
        std::vector<std::uint32_t> program(32, beq(0, 0, 0));
        program[0] = store ? sw(2, 0, 0) : i(3, 0, 0, true);
        program[1] = sw(2, 0, 4); // Must be squashed, then re-executed after MRET.
        program[16] = 0x30200073;
        auto cpu = pipeline(program); Bus bus; bus.words[0] = 123;
        cpu.set_register(2, 42); cpu.connect_bus(bus);
        cpu.interrupts().write(0x305, 64); cpu.interrupts().write(0x304, 1u << 11); cpu.interrupts().write(0x300, 8);
        while (!bus.pending) { bus.tick(); cpu.tick(); }
        cpu.interrupts().set_external(true);
        if (deassert) { bus.tick(); cpu.tick(); cpu.interrupts().set_external(false); }
        for (unsigned n = 0; n < 100 && cpu.completed_instructions() < 1; ++n) {
            bus.tick(); cpu.tick(); assert(cpu.interrupts().entries() == 0);
        }
        assert(cpu.completed_instructions() == 1 && cpu.pc() == 4 && bus.addresses.size() == 1);
        if (deassert) {
            retire_bus(cpu, bus, 1);
            assert(cpu.interrupts().entries() == 0 && bus.words[4] == 42);
        } else {
            bus.tick(); cpu.tick();
            assert(cpu.last_cycle()->interrupt_entry && cpu.pc() == 64 && cpu.interrupts().read(0x341) == 4);
            assert(bus.addresses.size() == 1 && (store ? bus.writes == 1 : cpu.registers().read(3) == 123));
            cpu.interrupts().set_external(false);
            retire_bus(cpu, bus, 1);
            assert(cpu.interrupts().returns() == 1 && cpu.pc() == 4);
            retire_bus(cpu, bus, 1);
            assert(bus.words[4] == 42 && bus.addresses.size() == 2);
        }
    }
    // An IRQ before any irreversible request saves the oldest unretired PC.
    std::vector<std::uint32_t> program(32, beq(0, 0, 0));
    program[0] = i(1, 0, 9); program[16] = 0x30200073;
    auto cpu = pipeline(program);
    cpu.interrupts().write(0x305, 64); cpu.interrupts().write(0x304, 1u << 11); cpu.interrupts().write(0x300, 8);
    for (unsigned n = 0; n < 4; ++n) cpu.tick();
    cpu.interrupts().set_external(true); cpu.tick();
    assert(cpu.pc() == 64 && cpu.registers().read(1) == 0 && cpu.interrupts().read(0x341) == 0);
    cpu.interrupts().set_external(false); cpu.run_instructions(2);
    assert(cpu.registers().read(1) == 9 && cpu.pc() == 4);
}

static void soc_and_mmu() {
    using namespace colab;
    SoC system({i(1, 0, 42), sw(1, 2, 0), i(3, 2, 0, true), r(4, 3, 1)});
    assert(system.cpu().execution() == Execution::Pipelined);
    system.cpu().set_register(2, 0x1000);
    for (unsigned n = 0; n < 1000 && system.cpu().completed_instructions() < 4; ++n) system.tick();
    assert(system.cpu().completed_instructions() == 4 && system.cpu().registers().read(4) == 84);
    assert(c::load32(system.memory().read(0x1000, 4)) == 42 && system.cpu().bus_waits() > 0);
    assert(system.data_cache().statistics().fills == 1);

    using namespace soc::core::mmu;
    SoC protected_cpu({sw(2, 1, 0), i(3, 1, 0, true)});
    protected_cpu.cpu().set_register(1, 0x1000); protected_cpu.cpu().set_register(2, 99);
    PageTablePort table(protected_cpu.memory().reader(), protected_cpu.memory().writer());
    PageTableEntry entry{2, true, {true, false, false, true}, true, false, 0};
    table.writer()(0x4008, entry);
    protected_cpu.cpu().mmu().enable_paging(0x4000, table.reader(), table.writer());
    bool caught = false;
    try { for (unsigned n = 0; n < 100; ++n) protected_cpu.tick(); }
    catch (const TranslationFault& error) { caught = true; assert(error.reason() == FaultReason::WriteDenied); }
    assert(caught && protected_cpu.cpu().completed_instructions() == 0 && protected_cpu.cpu().pc() == 0);
    assert(protected_cpu.bus().fabric().statistics().completed == 0);
    entry.permissions.writable = true;
    table.writer()(0x4008, entry); protected_cpu.cpu().mmu().invalidate_page(1);
    for (unsigned n = 0; n < 1000 && protected_cpu.cpu().completed_instructions() < 2; ++n) protected_cpu.tick();
    assert(protected_cpu.cpu().registers().read(3) == 99 && c::load32(protected_cpu.memory().read(0x2000, 4)) == 99);
}

static void differential() {
    std::mt19937 random(0x51A6E);
    for (bool forwarding : {false, true}) {
        std::vector<std::uint32_t> program;
        for (unsigned n = 0; n < 2400; ++n) {
            const auto rd = random() % 31, a = random() % 31, b = random() % 31;
            const auto offset = static_cast<int>(random() % 64) * 4 - 128;
            switch (random() % 6) {
            case 0: program.push_back(r(rd, a, b)); break;
            case 1: program.push_back(r(rd, a, b, true)); break;
            case 2: program.push_back(i(rd, a, static_cast<int>(random() % 4096) - 2048)); break;
            case 3: program.push_back(i(rd, 31, offset, true)); break;
            case 4: program.push_back(sw(b, 31, offset)); break;
            default: program.push_back(beq(a, random() % 2 ? a : b, 8)); break;
            }
        }
        program.push_back(i(0, 0, 0));
        auto cpu = pipeline(program, forwarding);
        CPU hard(program, Mode::Hardwired, 64), micro(program, Mode::Microprogrammed, 64);
        Reference reference;
        for (unsigned n = 1; n < 32; ++n) {
            const auto value = n == 31 ? 128u : random();
            cpu.set_register(n, value); hard.set_register(n, value); micro.set_register(n, value); reference.registers[n] = value;
        }
        for (unsigned address = 0; address < 256; address += 4) {
            const auto value = random(); cpu.set_memory(address, value); hard.set_memory(address, value); micro.set_memory(address, value);
            reference.memory[address / 4] = value;
        }
        while (reference.pc / 4 < program.size()) {
            const auto word = program[reference.pc / 4]; reference.step(word);
            const auto retired = cpu.step_instruction(); hard.step_instruction(); micro.step_instruction();
            assert(retired.instruction == word && retired.next_pc == reference.pc);
            compare_to_reference(cpu, reference); compare_architecture(cpu, hard); compare_architecture(cpu, micro);
        }
        assert(cpu.completed_instructions() > 1900);
    }
    auto loop = pipeline({i(1, 0, 3), i(1, 1, -1), beq(1, 0, 8), beq(0, 0, -8), i(2, 1, 9)});
    loop.run_instructions(10);
    assert(loop.registers().read(1) == 0 && loop.registers().read(2) == 9 && loop.pc() == 20);
}

int main() {
    timing_and_hazards(); waits_and_side_effects(); faults_and_csr(); interrupt_boundaries(); soc_and_mmu(); differential();
    std::cout << "PASS RV32 pipeline: stages/forwarding/stalls/flush, precise effects/IRQs/faults, SoC/MMU/cache, retirement oracle\n";
}
