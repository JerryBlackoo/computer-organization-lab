#include "../soc/core/rv32_core.hpp"
#include <iostream>

using CPU = colab::soc::RV32Core;
using Mode = CPU::Mode;
using Operation = colab::soc::core::Operation;
using Fault = colab::soc::core::RV32Fault;
using Cause = colab::soc::core::RV32FaultCause;
static void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
static std::uint32_t i(unsigned rd, unsigned rs1, int immediate) {
    return ((static_cast<std::uint32_t>(immediate) & 0xfffu) << 20) | (rs1 << 15) | (rd << 7) | 0x13u;
}
static std::uint32_t sw(unsigned rs2, unsigned rs1, int immediate) {
    const auto bits = static_cast<std::uint32_t>(immediate) & 0xfffu;
    return ((bits >> 5) << 25) | (rs2 << 20) | (rs1 << 15) | 0x2000u | ((bits & 31u) << 7) | 0x23u;
}
static std::uint32_t beq(unsigned rs1, unsigned rs2, int immediate) {
    const auto bits = static_cast<std::uint32_t>(immediate) & 0x1fffu;
    return ((bits >> 12) << 31) | (((bits >> 5) & 63u) << 25) | (rs2 << 20) | (rs1 << 15) |
        (((bits >> 1) & 15u) << 8) | (((bits >> 11) & 1u) << 7) | 0x63u;
}
static std::uint32_t jal(unsigned rd, int offset) {
    const auto bits = static_cast<std::uint32_t>(offset) & 0x1fffffu;
    return ((bits >> 20) << 31) | (((bits >> 1) & 0x3ffu) << 21) | (((bits >> 11) & 1u) << 20) |
        (((bits >> 12) & 0xffu) << 12) | (rd << 7) | 0x6fu;
}
static std::uint32_t jalr(unsigned rd, unsigned rs1, int offset) {
    return ((static_cast<std::uint32_t>(offset) & 0xfffu) << 20) | (rs1 << 15) | (rd << 7) | 0x67u;
}

int main() {
    using Execution = CPU::Execution;
    for (unsigned config = 0; config < 4; ++config) {
        const auto mode = config == 1 ? Mode::Microprogrammed : Mode::Hardwired;
        const auto execution = config >= 2 ? Execution::Pipelined : Execution::MultiCycle;
        const bool forwarding = config != 3;
        CPU cpu({0x123450b7, 0xfffff117, jal(5, 12), i(3, 0, 88), i(3, 0, 99),
                 i(5, 5, 17), jalr(5, 5, 0), i(6, 5, 0), beq(0, 0, 0)}, mode, 64, {}, execution, {forwarding});
        cpu.run_instructions(6, 1000);
        require(cpu.registers().read(1) == 0x12345000 && cpu.registers().read(2) == 0xfffff004,
                "LUI or AUIPC result mismatch");
        require(cpu.registers().read(3) == 0 && cpu.registers().read(5) == 28 && cpu.registers().read(6) == 28 &&
                cpu.pc() == 32, "JAL/JALR, forwarding or wrong-path suppression failed");
        CPU loop({i(1, 1, 1), jal(0, -4), i(2, 0, 7)}, mode, 64, {}, execution, {forwarding});
        loop.run_instructions(4, 1000);
        require(loop.pc() == 0 && loop.registers().read(1) == 2 && loop.registers().read(0) == 0 &&
                loop.registers().read(2) == 0, "backward JAL/x0 semantics failed");
        CPU dependent({i(1, 0, 17), jalr(1, 1, 0), i(3, 0, 9), sw(3, 0, 0),
                       i(4, 1, 0), beq(0, 0, 0)}, mode, 64, {}, execution, {forwarding});
        dependent.run_instructions(3, 1000);
        require(dependent.registers().read(1) == 8 && dependent.registers().read(4) == 8 &&
                dependent.registers().read(3) == 0 && dependent.memory().read(0) == 0,
                "dependent indirect redirect failed to forward/flush precisely");
        for (bool indirect : {false, true}) {
            CPU fault({indirect ? jalr(1, 1, 0) : jal(1, 2), i(2, 0, 9)}, mode, 64, {}, execution, {forwarding});
            fault.set_register(1, 3);
            bool trapped = false;
            try { fault.run_instructions(1, 1000); }
            catch (const Fault& error) {
                trapped = error.cause() == Cause::InstructionAddressMisaligned && error.pc() == 0 && error.value() == 2;
            }
            require(trapped && fault.pc() == 0 && fault.registers().read(1) == 3 &&
                    fault.completed_instructions() == 0, "misaligned jump had architectural side effects");
        }
    }
    bool invalid = false;
    for (const auto offset : {-1048576, -65538, -4096, -2, 0, 2, 4096, 65536, 1048574}) {
        const auto decoded = colab::soc::core::Decoder{}.decode_rv32(jal(7, offset));
        require(decoded.operation == Operation::Jal && decoded.immediate == offset, "JAL immediate decode failed");
    }
    try { (void)colab::soc::core::Decoder{}.decode_rv32(0x00001067); }
    catch (const std::invalid_argument&) { invalid = true; }
    require(invalid, "invalid JALR funct3 accepted");
    std::cout << "PASS RV32 LUI/AUIPC/JAL/JALR: 3 execution modes, forwarding, redirects and precise faults\n";
}
