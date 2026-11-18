#include "../soc/rv32/core.hpp"
#include <iostream>

using CPU = colab::soc::RV32Core;
using Operation = colab::soc::core::Operation;
using Fault = colab::soc::core::RV32Fault;
using Cause = colab::soc::core::RV32FaultCause;
static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static std::uint32_t encode(unsigned funct3, unsigned rd, unsigned source, unsigned csr = 0x340) {
    return (csr << 20) | (source << 15) | (funct3 << 12) | (rd << 7) | 0x73;
}
static CPU make_cpu(std::vector<std::uint32_t> program, unsigned config,
                    colab::soc::core::FaultHandling policy = colab::soc::core::FaultHandling::HostException) {
    return CPU(std::move(program), config == 1 ? CPU::Mode::Microprogrammed : CPU::Mode::Hardwired, 64, {},
        config >= 2 ? CPU::Execution::Pipelined : CPU::Execution::MultiCycle, {config != 3}, policy);
}
struct Probe {
    std::uint32_t value = 0xa5a55a5a;
    unsigned reads = 0, writes = 0, checks = 0;
    bool writable = false;
    void check_guest_csr(std::uint16_t csr, bool write) { require(csr == 0x340, "CSR address lost"); ++checks; writable = write; }
    std::uint32_t read(std::uint16_t) { ++reads; return value; }
    void write(std::uint16_t, std::uint32_t next) { ++writes; value = next; }
};
int main() {
    colab::soc::core::Decoder decoder;
    for (unsigned funct3 : {1u, 2u, 3u, 5u, 6u, 7u}) for (unsigned rd : {0u, 3u})
        for (unsigned source : {0u, 1u, 31u}) for (std::uint32_t register_value : {0u, 0xffffffffu, 0x12345678u}) {
            const auto word = encode(funct3, rd, source);
            const auto instruction = decoder.decode_rv32(word);
            const bool swap = funct3 % 4 == 1;
            const bool clear = funct3 % 4 == 3;
            const bool immediate = funct3 >= 5;
            const auto operand = immediate ? source : source ? register_value : 0u;
            const std::uint32_t before = 0xa5a55a5a;
            const auto expected = swap ? operand : clear ? (before & ~operand) : (before | operand);
            Probe probe;
            const auto result = colab::soc::core::access_csr(probe, instruction, source ? register_value : 0u);
            require(probe.checks == 1 && probe.reads == unsigned(!swap || rd != 0) &&
                probe.writes == unsigned(swap || source != 0) && probe.writable == (swap || source != 0),
                "CSR suppressed read/write still produced a side effect");
            require(probe.value == expected && result == ((!swap || rd) ? before : 0u), "CSR RMW value differed");
            for (unsigned config = 0; config < 4; ++config) {
                auto cpu = make_cpu({word}, config);
                cpu.interrupts().write(0x340, before); cpu.set_register(source, register_value);
                cpu.step_instruction(1000);
                require(cpu.interrupts().read(0x340) == expected && cpu.registers().read(rd) == (rd ? before : 0u) &&
                    cpu.pc() == 4 && bool(cpu.last_retirement()->register_write) == (rd != 0),
                    "CPU CSR operation or retirement differed");
            }
        }
    for (unsigned config = 0; config < 4; ++config) {
        // A source/destination alias uses the source value from before CSR writeback.
        auto alias = make_cpu({encode(3, 1, 1)}, config); alias.set_register(1, 0xff);
        alias.interrupts().write(0x340, 0x12345678); alias.step_instruction(1000);
        require(alias.registers().read(1) == 0x12345678 && alias.interrupts().read(0x340) == 0x12345600,
            "CSR operand/destination alias lost old source");
        for (unsigned funct3 : {1u, 2u, 3u, 5u, 6u, 7u}) for (unsigned source : {0u, 1u}) {
            auto cpu = make_cpu({encode(funct3, 3, source, 0xf14)}, config);
            cpu.set_register(3, 99); // Nonzero rs1 with a zero value must still attempt a write.
            bool faulted = false;
            try { cpu.step_instruction(1000); }
            catch (const Fault& fault) { faulted = fault.cause() == Cause::IllegalInstruction && fault.value() == encode(funct3, 3, source, 0xf14); }
            const bool write = funct3 % 4 == 1 || source != 0;
            require(faulted == write && cpu.completed_instructions() == unsigned(!write) &&
                cpu.registers().read(3) == (write ? 99u : 0u), "read-only CSR access suppression/precision differed");
        }
        auto misa = make_cpu({encode(1, 2, 1, 0x301)}, config); misa.set_register(1, 0xffffffff); misa.step_instruction(1000);
        require(misa.registers().read(2) == 0x40141100 && misa.interrupts().read(0x301) == 0x40141100, "misa WARL value changed");
        for (unsigned funct3 : {1u, 2u, 3u, 5u, 6u, 7u}) {
            std::vector<std::uint32_t> program(64, 0x00000063); program[0] = 0x30200073;
            program[16] = encode(funct3, 3, 1);
            auto cpu = make_cpu(program, config, colab::soc::core::FaultHandling::MachineTrap);
            cpu.interrupts().write(0x3b0, 0x1fffffff); cpu.interrupts().write(0x3a0, 0x1f);
            cpu.interrupts().write(0x305, 128); cpu.interrupts().write(0x341, 64); cpu.interrupts().write(0x300, 0);
            for (unsigned n = 0; n < 1000 && !cpu.interrupts().exceptions(); ++n) cpu.tick();
            require(cpu.interrupts().read(0x342) == 2 && cpu.interrupts().read(0x341) == 64 &&
                cpu.interrupts().read(0x343) == program[16] && cpu.completed_instructions() == 1,
                "new CSR form bypassed U-mode privilege check");
        }
    }
    for (bool forwarding : {false, true}) {
        CPU cpu({0x02002f83, encode(5, 2, 31), 0x00100193}, CPU::Mode::Hardwired, 64, {}, CPU::Execution::Pipelined, {forwarding});
        cpu.set_memory(32, 0x12345678);
        unsigned false_dependencies = 0;
        for (unsigned n = 0; n < 100 && cpu.completed_instructions() < 3; ++n) {
            cpu.tick(); false_dependencies += cpu.last_pipeline_cycle()->data_stall;
        }
        require(cpu.completed_instructions() == 3 && !false_dependencies && cpu.interrupts().read(0x340) == 31,
            "CSR immediate treated its uimm field as a register dependency");
    }
    bool rejected = false;
    try { (void)decoder.decode_rv32(encode(4, 0, 0)); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "reserved SYSTEM funct3 accepted");
    std::cout << "PASS Zicsr: all six forms, independent values, read/write suppression, aliasing, read-only CSRs and M/U privilege\n";
}
