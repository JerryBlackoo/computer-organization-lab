#include "../soc/rv32/core.hpp"
#include <iostream>

using CPU = colab::soc::RV32Core;
using Fault = colab::soc::core::RV32Fault;
using Cause = colab::soc::core::RV32FaultCause;
static void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
static std::uint32_t branch(unsigned funct3, unsigned a, unsigned b, int offset) {
    const auto imm = static_cast<std::uint32_t>(offset) & 0x1fffu;
    return ((imm >> 12) << 31) | (((imm >> 5) & 63u) << 25) | (b << 20) | (a << 15) |
        (funct3 << 12) | (((imm >> 1) & 15u) << 8) | (((imm >> 11) & 1u) << 7) | 0x63u;
}
static std::uint32_t addi(unsigned rd, unsigned rs, int imm) {
    return ((static_cast<std::uint32_t>(imm) & 0xfffu) << 20) | (rs << 15) | (rd << 7) | 0x13u;
}
int main() {
    struct Case { unsigned funct3; std::uint32_t a, b; bool taken; };
    const Case cases[] = {
        {0, 5, 5, true}, {0, 5, 6, false}, {1, 5, 6, true}, {1, 5, 5, false},
        {4, 0xffffffff, 1, true}, {4, 1, 0xffffffff, false}, {4, 0x80000000, 0x7fffffff, true},
        {5, 1, 0xffffffff, true}, {5, 0xffffffff, 1, false}, {5, 9, 9, true},
        {6, 0xffffffff, 1, false}, {6, 1, 0xffffffff, true}, {6, 0, 0xffffffff, true},
        {7, 0xffffffff, 1, true}, {7, 1, 0xffffffff, false}, {7, 9, 9, true}
    };
    for (unsigned config = 0; config < 4; ++config) {
        const auto mode = config == 1 ? CPU::Mode::Microprogrammed : CPU::Mode::Hardwired;
        const auto execution = config >= 2 ? CPU::Execution::Pipelined : CPU::Execution::MultiCycle;
        for (const auto& test : cases) {
            CPU cpu({branch(test.funct3, 1, 2, 8), addi(3, 0, 7), addi(4, 0, 9)}, mode, 64, {}, execution, {config != 3});
            cpu.set_register(1, test.a); cpu.set_register(2, test.b); cpu.step_instruction(1000);
            require(cpu.pc() == (test.taken ? 8u : 4u) && cpu.registers().read(1) == test.a &&
                    cpu.registers().read(2) == test.b, "conditional branch result mismatch");
        }
        CPU hazard({addi(1, 0, -1), addi(2, 0, 1), branch(4, 1, 2, 8), 0x00202023,
                    addi(3, 0, 9), branch(0, 0, 0, 0)}, mode, 64, {}, execution, {config != 3});
        hazard.run_instructions(4, 1000);
        require(hazard.registers().read(3) == 9 && hazard.memory().read(0) == 0,
                "branch forwarding or wrong-path store suppression failed");
        for (const bool taken : {false, true}) {
            CPU cpu({branch(1, 1, 2, 2), addi(3, 0, 9)}, mode, 64, {}, execution, {config != 3});
            cpu.set_register(1, 1); cpu.set_register(2, taken ? 2 : 1);
            bool trapped = false;
            try { cpu.step_instruction(1000); }
            catch (const Fault& e) { trapped = e.cause() == Cause::InstructionAddressMisaligned && e.pc() == 0 && e.value() == 2; }
            require(trapped == taken && cpu.pc() == (taken ? 0u : 4u) &&
                    cpu.completed_instructions() == (taken ? 0u : 1u), "branch alignment precision failed");
        }
    }
    for (unsigned funct3 : {2u, 3u}) {
        bool invalid = false;
        try { (void)colab::soc::core::Decoder{}.decode_rv32(branch(funct3, 1, 2, 8)); }
        catch (const std::invalid_argument&) { invalid = true; }
        require(invalid, "reserved branch encoding accepted");
    }
    std::cout << "PASS RV32 conditional branches: signed/unsigned comparisons, hazards and precise faults\n";
}
