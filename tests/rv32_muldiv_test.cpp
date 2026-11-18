#include "../soc/rv32/core.hpp"
#include "../soc/shared/control_unit/control_signal_generator/control_signal_generator.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>

using CPU = colab::soc::RV32Core;
using namespace colab::soc::core;
using namespace colab::soc::core::datapath::dedicated;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static std::int64_t signed_value(std::uint32_t bits) {
    return static_cast<std::int64_t>(bits) - ((bits >> 31) ? 0x100000000ll : 0);
}
static std::uint32_t reference(unsigned kind, std::uint32_t a, std::uint32_t b) {
    const auto signed_a = signed_value(a), signed_b = signed_value(b);
    switch (kind) {
    case 0: return static_cast<std::uint32_t>(std::uint64_t{a} * b);
    case 1: return static_cast<std::uint32_t>(static_cast<std::uint64_t>(signed_a * signed_b) >> 32);
    case 2: return static_cast<std::uint32_t>(static_cast<std::uint64_t>(signed_a * std::int64_t{b}) >> 32);
    case 3: return static_cast<std::uint32_t>((std::uint64_t{a} * b) >> 32);
    case 4: return b ? static_cast<std::uint32_t>(signed_a / signed_b) : UINT32_MAX;
    case 5: return b ? a / b : UINT32_MAX;
    case 6: return b ? static_cast<std::uint32_t>(signed_a % signed_b) : a;
    case 7: return b ? a % b : a;
    }
    throw std::logic_error("unknown reference operation");
}
static void equal(std::uint32_t actual, std::uint32_t expected, unsigned kind,
                  std::uint32_t a, std::uint32_t b, const char* context) {
    if (actual == expected) return;
    std::ostringstream out;
    out << context << " funct3=" << kind << std::hex << " a=" << a << " b=" << b
        << " actual=" << actual << " expected=" << expected;
    throw std::runtime_error(out.str());
}
static std::uint32_t m(unsigned kind, unsigned rd, unsigned rs1, unsigned rs2) {
    return (1u << 25) | (rs2 << 20) | (rs1 << 15) | (kind << 12) | (rd << 7) | 0x33u;
}
static std::uint32_t addi(unsigned rd, unsigned rs1, int immediate) {
    return ((static_cast<std::uint32_t>(immediate) & 0xfffu) << 20) | (rs1 << 15) | (rd << 7) | 0x13u;
}
static CPU make_cpu(std::vector<std::uint32_t> program, unsigned config,
                    FaultHandling handling = FaultHandling::HostException) {
    return CPU(std::move(program), config == 1 ? CPU::Mode::Microprogrammed : CPU::Mode::Hardwired, 64, {},
        config >= 2 ? CPU::Execution::Pipelined : CPU::Execution::MultiCycle, {config != 3}, handling);
}
static constexpr std::array<Operation, 8> operations{
    Operation::Mul, Operation::Mulh, Operation::Mulhsu, Operation::Mulhu,
    Operation::Div, Operation::Divu, Operation::Rem, Operation::Remu};
static constexpr std::array<ALUCtr, 8> controls{
    ALUCtr::Mul, ALUCtr::Mulh, ALUCtr::Mulhsu, ALUCtr::Mulhu,
    ALUCtr::Div, ALUCtr::Divu, ALUCtr::Rem, ALUCtr::Remu};
static constexpr std::array<std::uint32_t, 13> edges{
    0, 1, 2, 3, 0x7fffffffu, 0x80000000u, 0x80000001u,
    0xfffffffeu, 0xffffffffu, 0x55555555u, 0xaaaaaaaau, 0x00010000u, 0xffff0000u};

static void test_decode_and_control() {
    Decoder decoder;
    control_unit::ControlSignalGenerator hardwired;
    control_unit::microprogrammed::ControlStore micro;
    for (unsigned kind = 0; kind < operations.size(); ++kind) {
        const auto decoded = decoder.decode_rv32(m(kind, 31, 30, 29));
        require(decoded.operation == operations[kind] && decoded.rd == 31 && decoded.rs1 == 30 && decoded.rs2 == 29 &&
                decoded.opcode == 0x33 && decoded.funct7 == 1 && decoded.funct3 == kind && is_register_alu(decoded.operation),
                "M instruction encoding or dependency classification differed");
        const auto signal = hardwired.generate(control_unit::rv32::Phase::Execute, decoded, {});
        require(signal.alu_op == controls[kind] && signal.muldiv_start && !signal.muldiv_step && !signal.alu_in &&
                signal.operand_b == control_unit::rv32::OperandB::Register, "hardwired execute controls differed");
        const auto running = hardwired.generate(control_unit::rv32::Phase::Execute, decoded, {false, true, false});
        require(!running.muldiv_start && running.muldiv_step && !running.alu_in, "hardwired M iteration controls differed");
        const auto address = micro.entry(decoded.operation);
        require(address == 200 + kind * 4 && micro.at(address).control.operands_in &&
                micro.at(address + 1).control.muldiv_start && micro.at(address + 1).control.alu_op == controls[kind] &&
                micro.at(address + 2).control.muldiv_step &&
                micro.at(address + 3).control.register_write && micro.at(address + 3).control.retire,
                "microprogram dispatch or controls differed");
    }
    for (unsigned kind = 0; kind < 8; ++kind) for (unsigned funct7 : {2u, 0x7fu}) {
        bool rejected = false;
        try { (void)decoder.decode_rv32((m(kind, 3, 1, 2) & 0x01ffffffu) | (funct7 << 25)); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "reserved R-type encoding accepted");
    }
}

static void test_clocked_unit() {
    MulDiv unit;
    bool rejected = false;
    try { (void)unit.result(); } catch (const std::logic_error&) { rejected = true; }
    require(rejected, "idle unit exposed a result");
    unit.start(ALUCtr::Mul, 3, 5);
    require(unit.busy() && !unit.done() && unit.iteration() == 0 && unit.partial() == 0 && unit.q() == 5,
            "unsigned multiplication initialization differed");
    rejected = false;
    try { unit.start(ALUCtr::Mul, 9, 9); } catch (const std::logic_error&) { rejected = true; }
    require(rejected, "busy unit accepted another instruction");
    unit.step();
    require(unit.beat() == 1 && unit.iteration() == 1 && unit.partial() == 1 && unit.q() == 0x80000002u &&
            unit.last_action() == MulDiv::Action::Add && unit.alu_used() && unit.alu_a() == 0 &&
            unit.alu_b() == 3 && unit.alu_result() == 3, "unsigned first ALU/shift beat differed");
    unit.step();
    require(unit.iteration() == 2 && unit.partial() == 0 && unit.q() == 0xc0000001u && !unit.alu_used(),
            "unsigned second iteration differed");
    while (unit.busy()) unit.step();
    require(unit.done() && unit.iteration() == 32 && unit.beat() == 32 && unit.result() == 15 && unit.product() == 15,
            "unsigned multiplication did not finish after 32 beats");
    unit.start(ALUCtr::Mulhu, 0xffffffffu, 0xffffffffu);
    unit.step(); unit.step();
    require(unit.carry() && unit.partial() == 0xbfffffffu && unit.q() == 0x7fffffffu,
            "unsigned multiplication lost the carry during shift");
    while (unit.busy()) unit.step();
    unit.start(ALUCtr::Mulh, 0xfffffffdu, 5); // -3 * 5, Booth.
    unit.step();
    require(unit.partial() == 1 && unit.q() == 0x80000002u && unit.previous_bit() &&
            unit.last_action() == MulDiv::Action::Subtract && unit.alu_control() == ALUCtr::Subu,
            "first Booth shift beat differed");
    unit.step();
    require(unit.partial() == 0x1ffffffffull && unit.q() == 0x40000001u && !unit.previous_bit() &&
            unit.last_action() == MulDiv::Action::Add,
            "second Booth shift beat differed");
    while (unit.busy()) unit.step();
    require(unit.product() == 0xfffffffffffffff1ull && unit.result() == 0xffffffffu, "Booth product differed");
    unit.start(ALUCtr::Mulhsu, 0xfffffffdu, 0xffffffffu);
    while (unit.busy()) unit.step();
    require(unit.beat() == 33 && unit.product() == 0xfffffffd00000003ull && unit.partial() == 0x1fffffffdull && unit.q() == 3,
            "mixed-sign multiplication final P:Q did not reflect correction");
    unit.start(ALUCtr::Divu, 0xd0000000u, 3);
    unit.step();
    require(unit.partial() == 1 && unit.q() == 0xa0000000u && unit.last_action() == MulDiv::Action::Restore,
            "first division restore beat differed");
    unit.step();
    require(unit.partial() == 0 && unit.q() == 0x40000001u, "second restoring division step differed");
    while (unit.busy()) unit.step();
    require(unit.result() == 0xd0000000u / 3u && unit.division().remainder == 0xd0000000u % 3u,
            "restoring division result differed");
    unit.start(ALUCtr::Div, 0xfffffff3u, 3); // -13 / 3 = -4, remainder -1.
    while (unit.busy()) unit.step();
    require(unit.beat() == 34 && unit.q() == 0xfffffffcu && unit.partial() == 0x1ffffffffull &&
            unit.result() == 0xfffffffcu && unit.division().remainder == 0xffffffffu,
            "signed division final registers did not reflect sign correction");
    unit.start(ALUCtr::Rem, 0x80000000u, 0);
    require(unit.done() && !unit.busy() && unit.iteration() == 0 && unit.result() == 0x80000000u &&
            unit.q() == UINT32_MAX && unit.partial() == 0x180000000ull,
            "divide by zero fast path differed");
    unit.start(ALUCtr::Div, 0x80000000u, 0xffffffffu);
    require(unit.done() && !unit.busy() && unit.result() == 0x80000000u && unit.division().remainder == 0,
            "signed overflow fast path differed");
}

static void test_shared_alu() {
    struct Probe {
        mutable unsigned calls = 0;
        datapath::dedicated::ALU alu;
        datapath::dedicated::ALUResult evaluate(ALUCtr control, std::uint32_t a, std::uint32_t b) const {
            ++calls;
            require(control == ALUCtr::Addu || control == ALUCtr::Subu, "MDU selected a non-add/sub ALU function");
            return alu.evaluate(control, a, b);
        }
    };
    for (unsigned kind = 0; kind < 8; ++kind) for (auto a : edges) for (auto b : edges) {
        Probe probe;
        MulDiv unit; unit.start(controls[kind], a, b);
        while (unit.busy()) {
            const auto before = probe.calls;
            unit.step(probe);
            require(probe.calls - before <= 1 && unit.alu_used() == (probe.calls != before),
                    "MDU used more than one ALU operation in a beat or failed to expose it");
        }
        equal(unit.result(), reference(kind, a, b), kind, a, b, "shared ALU MDU");
    }
}

static void test_clocked_cpu(unsigned config) {
    auto cpu = make_cpu({m(0, 3, 1, 2), addi(4, 3, 1), 0x00402023u}, config);
    cpu.set_register(1, 3); cpu.set_register(2, 5); cpu.set_register(3, 0xabcdu);
    unsigned observed = 0, stalls = 0;
    for (unsigned cycle = 0; cycle < 100 && !cpu.completed_instructions(); ++cycle) {
        cpu.tick();
        const auto& trace = *cpu.last_cycle();
        if (trace.muldiv) {
            const auto& unit = *trace.muldiv;
            require(unit.beat() == observed && unit.iteration() == observed &&
                    unit.operand_a() == 3 && unit.operand_b() == 5, "CPU did not expose consecutive M beats");
            require(unit.done() == (observed == 32) && unit.busy() == (observed < 32),
                    "CPU M busy/done timing differed");
            require(cpu.registers().read(3) == 0xabcdu && cpu.registers().read(4) == 0 && cpu.memory().read(0) == 0,
                    "M or a younger instruction wrote before retirement");
            if (config < 2) require(cpu.state().alu_out == (unit.done() ? 15u : 0u), "ALUOut changed before M done");
            if (config >= 2) stalls += cpu.last_pipeline_cycle()->muldiv_stall;
            ++observed;
        }
    }
    const std::uint64_t expected_cycles = config == 0 ? 42 : config == 1 ? 43 : 37;
    require(observed == 33 && cpu.completed_instructions() == 1 && cpu.clock_pulses() == expected_cycles &&
            cpu.registers().read(3) == 15 && cpu.pc() == 4, "M execution or retirement timing differed");
    if (config >= 2) require(stalls == 32, "pipeline did not hold for every unfinished M beat");
    cpu.step_instruction(1000);
    require(cpu.registers().read(4) == 16, "M dependent instruction did not receive completed result");
    cpu.step_instruction(1000);
    require(cpu.memory().read(0) == 16, "store after M did not retire with the completed result");
}

static void test_interrupt_during_muldiv(unsigned config, unsigned interrupt_beat) {
    std::vector<std::uint32_t> program(32, 0x00000013u);
    program[0] = m(0, 3, 1, 2); program[1] = addi(4, 3, 1); program[16] = 0x30200073u;
    auto cpu = make_cpu(program, config, FaultHandling::MachineTrap);
    cpu.set_register(1, 7); cpu.set_register(2, 3); cpu.set_register(3, 0xabcdu);
    cpu.interrupts().write(0x305, 64); cpu.interrupts().write(0x304, 1u << 11); cpu.interrupts().write(0x300, 8);
    bool running = false;
    for (unsigned cycle = 0; cycle < 100 && !running; ++cycle) {
        cpu.tick();
        running = cpu.last_cycle()->muldiv && cpu.last_cycle()->muldiv->beat() == interrupt_beat;
    }
    require(running && cpu.registers().read(3) == 0xabcdu, "IRQ fixture failed to start M");
    cpu.interrupts().set_external(true);
    for (unsigned cycle = 0; cycle < 100 && !cpu.interrupts().entries(); ++cycle) cpu.tick();
    require(cpu.interrupts().entries() == 1 && cpu.pc() == 64 && !cpu.interrupts().exceptions(),
            "IRQ during M did not enter correctly");
    if (config < 2) {
        require(cpu.registers().read(3) == 21 && cpu.interrupts().read(0x341) == 4,
                "multicycle IRQ interrupted M before retirement");
    } else {
        require(cpu.registers().read(3) == 0xabcdu && cpu.interrupts().read(0x341) == 0 &&
                !cpu.pipeline().muldiv().busy(), "pipeline IRQ failed to squash the unfinished M");
    }
    cpu.interrupts().set_external(false);
    cpu.step_instruction(1000); // MRET.
    if (config >= 2) cpu.step_instruction(1000); // Retry the squashed M.
    cpu.step_instruction(1000);
    require(cpu.registers().read(3) == 21 && cpu.registers().read(4) == 22 && cpu.interrupts().returns() == 1,
            "M did not resume correctly after IRQ");
}

static void test_pipeline_fault_squashes_muldiv(unsigned config) {
    std::vector<std::uint32_t> program(32, 0x00000013u);
    program[0] = 0xffffffffu; program[1] = m(0, 3, 1, 2);
    auto cpu = make_cpu(program, config, FaultHandling::MachineTrap);
    cpu.set_register(1, 7); cpu.set_register(2, 3); cpu.set_register(3, 0xabcdu);
    cpu.interrupts().write(0x305, 64);
    bool started = false;
    for (unsigned cycle = 0; cycle < 100 && !cpu.interrupts().exceptions(); ++cycle) {
        cpu.tick();
        started |= bool(cpu.last_cycle()->muldiv);
    }
    require(started && cpu.interrupts().exceptions() == 1 && cpu.interrupts().read(0x341) == 0 &&
            cpu.pc() == 64 && cpu.registers().read(3) == 0xabcdu && !cpu.pipeline().muldiv().busy(),
            "older fault failed to squash unfinished pipeline M");
}

static void test_alu_pair(std::uint32_t a, std::uint32_t b) {
    datapath::dedicated::ALU alu;
    for (unsigned kind = 0; kind < controls.size(); ++kind) {
        const auto actual = alu.evaluate(controls[kind], a, b);
        equal(actual.result, reference(kind, a, b), kind, a, b, "ALU");
        require(actual.zero == (actual.result == 0) && !actual.overflow, "M ALU status differed");
    }
    // Check both halves of each 64-bit datapath, independently of instruction selection.
    require(MulDiv::multiply_unsigned(a, b) == std::uint64_t{a} * b, "full unsigned product differed");
    require(MulDiv::multiply_signed(a, b) == static_cast<std::uint64_t>(signed_value(a) * signed_value(b)),
            "full Booth signed product differed");
    require(MulDiv::multiply_signed_unsigned(a, b) == static_cast<std::uint64_t>(signed_value(a) * std::int64_t{b}),
            "full signed-unsigned product differed");
}

static void test_core_pair(std::uint32_t a, std::uint32_t b, unsigned config) {
    std::vector<std::uint32_t> program;
    for (unsigned kind = 0; kind < 8; ++kind) program.push_back(m(kind, 8 + kind, 1, 2));
    auto cpu = make_cpu(program, config);
    cpu.set_register(1, a); cpu.set_register(2, b);
    for (unsigned kind = 0; kind < 8; ++kind) {
        const auto retired = cpu.step_instruction(1000);
        equal(cpu.registers().read(8 + kind), reference(kind, a, b), kind, a, b, "CPU");
        require(retired.pc == kind * 4 && retired.next_pc == (kind + 1) * 4 &&
                retired.register_write && retired.register_write->index == 8 + kind, "M retirement differed");
    }
}

static void test_division_edges() {
    struct Case { unsigned kind; std::uint32_t dividend, divisor, expected; };
    constexpr std::array cases{
        Case{4, 0xfffffff9u, 0, 0xffffffffu}, Case{5, 0x87654321u, 0, 0xffffffffu},
        Case{6, 0xfffffff9u, 0, 0xfffffff9u}, Case{7, 0x87654321u, 0, 0x87654321u},
        Case{4, 0, 0, 0xffffffffu}, Case{6, 0, 0, 0},
        Case{4, 0x80000000u, 0xffffffffu, 0x80000000u}, Case{6, 0x80000000u, 0xffffffffu, 0},
        Case{4, 0x80000000u, 1, 0x80000000u},
        Case{4, 0xfffffff9u, 3, 0xfffffffeu}, Case{6, 0xfffffff9u, 3, 0xffffffffu},
        Case{4, 7, 0xfffffffdu, 0xfffffffeu}, Case{6, 7, 0xfffffffdu, 1},
        Case{4, 0xfffffff9u, 0xfffffffdu, 2}, Case{6, 0xfffffff9u, 0xfffffffdu, 0xffffffffu},
        Case{5, 0xffffffffu, 1, 0xffffffffu}, Case{7, 0xffffffffu, 1, 0},
        Case{5, 0xffffffffu, 0x80000000u, 1}, Case{7, 0xffffffffu, 0x80000000u, 0x7fffffffu}};

    for (const auto& item : cases) for (unsigned config = 0; config < 4; ++config)
        for (auto handling : {FaultHandling::HostException, FaultHandling::MachineTrap}) for (unsigned rd : {0u, 3u}) {
            auto cpu = make_cpu({m(item.kind, rd, 1, 2), addi(4, 0, 7)}, config, handling);
            cpu.set_register(1, item.dividend); cpu.set_register(2, item.divisor); cpu.set_register(3, 0xdecafbadu);
            cpu.interrupts().write(0x341, 0x40); cpu.interrupts().write(0x342, 0x12345678);
            cpu.interrupts().write(0x343, 0x87654321);
            const auto first = cpu.step_instruction(1000);
            require(first.pc == 0 && first.next_pc == 4 && bool(first.register_write) == (rd != 0),
                    "division edge case did not retire normally");
            if (rd) equal(cpu.registers().read(rd), item.expected, item.kind, item.dividend, item.divisor, "division edge");
            else require(cpu.registers().read(0) == 0 && cpu.registers().read(3) == 0xdecafbadu,
                         "division edge write to x0 had a side effect");
            cpu.step_instruction(1000);
            require(cpu.pc() == 8 && cpu.completed_instructions() == 2 && cpu.registers().read(4) == 7 &&
                    cpu.interrupts().exceptions() == 0 && cpu.interrupts().entries() == 0 &&
                    cpu.interrupts().read(0x341) == 0x40 && cpu.interrupts().read(0x342) == 0x12345678 &&
                    cpu.interrupts().read(0x343) == 0x87654321, "division edge case incorrectly trapped");
        }
}

static void test_dependencies(unsigned config) {
    // LW -> MUL reads a just-loaded rs2. Other M operations both consume and
    // produce dependencies, including rd=rs1, rd=rs2 and writes to x0.
    const std::vector<std::uint32_t> program{
        addi(1, 0, -21), addi(2, 0, 4), m(0, 3, 1, 2), m(4, 4, 3, 2),
        m(6, 5, 3, 2), m(1, 6, 1, 2), m(2, 7, 1, 2), m(3, 8, 1, 2),
        m(5, 9, 3, 2), m(7, 10, 3, 2), addi(2, 0, 7), m(0, 11, 4, 2),
        m(0, 1, 1, 2), m(0, 2, 1, 2), m(0, 0, 1, 2),
        0x00002703u, m(0, 15, 2, 14), 0x00f02223u, 0x00402803u, m(0, 17, 16, 0)};
    auto cpu = make_cpu(program, config);
    cpu.set_memory(0, 3);
    bool stalled = false, forwarded_mul_rs2 = false;
    for (unsigned cycle = 0; cycle < 3000 && cpu.completed_instructions() < program.size(); ++cycle) {
        cpu.tick();
        if (config >= 2) {
            const auto& trace = *cpu.last_pipeline_cycle();
            stalled |= trace.data_stall;
            forwarded_mul_rs2 |= trace.pc[2] && *trace.pc[2] == 8 && trace.forward_b != pipeline::Forward::Register;
        }
    }
    require(cpu.completed_instructions() == program.size() && cpu.pc() == program.size() * 4,
            "dependency program did not retire");
    const std::array<std::pair<unsigned, std::uint32_t>, 17> expected{{
        {0, 0}, {1, 0xffffff6du}, {2, 0xfffffbfbu}, {3, 0xffffffacu}, {4, 0xffffffebu},
        {5, 0}, {6, 0xffffffffu}, {7, 0xffffffffu}, {8, 3}, {9, 0x3fffffebu}, {10, 0},
        {11, 0xffffff6du}, {14, 3}, {15, 0xfffff3f1u}, {16, 0xfffff3f1u}, {17, 0}, {31, 0}}};
    for (const auto& [reg, value] : expected) require(cpu.registers().read(reg) == value, "M dependency result differed");
    require(cpu.memory().read(4) == 0xfffff3f1u, "M result did not reach store");
    if (config >= 2) require(stalled, "M load-use dependency failed to stall");
    if (config == 2) require(forwarded_mul_rs2, "M rs2 did not receive forwarding");
}

int main() {
    test_decode_and_control();
    test_clocked_unit();
    test_shared_alu();
    for (unsigned config = 0; config < 4; ++config) {
        test_clocked_cpu(config);
        for (unsigned beat : {7u, 8u}) test_interrupt_during_muldiv(config, beat);
        if (config >= 2) test_pipeline_fault_squashes_muldiv(config);
    }
    test_division_edges();
    for (auto a : edges) for (auto b : edges) {
        test_alu_pair(a, b);
        for (unsigned config = 0; config < 4; ++config) test_core_pair(a, b, config);
    }
    std::mt19937 random(0x4d455854u);
    for (unsigned sample = 0; sample < 20000; ++sample) {
        const auto a = random(), b = random();
        test_alu_pair(a, b);
        if (sample < 128) for (unsigned config = 0; config < 4; ++config) test_core_pair(a, b, config);
    }
    for (unsigned config = 0; config < 4; ++config) test_dependencies(config);
    std::cout << "PASS RV32M: 8 encodings, division edge cases without traps, 20169 reference pairs, 4 CPU configurations, aliases and hazards\n";
}
