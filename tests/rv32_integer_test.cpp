#include "../soc/rv32/core.hpp"
#include <iostream>
#include <random>

using CPU = colab::soc::RV32Core;
static void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
static std::uint32_t encode(unsigned kind, bool immediate, unsigned rd, unsigned rs1, std::uint32_t operand) {
    const unsigned funct3[]{7, 6, 4, 1, 5, 5, 2, 3};
    if (!immediate) return ((kind == 5 ? 0x20u : 0u) << 25) | (operand << 20) |
        (rs1 << 15) | (funct3[kind] << 12) | (rd << 7) | 0x33u;
    const auto imm = (kind >= 3 && kind <= 5) ? ((operand & 31u) | (kind == 5 ? 0x400u : 0u)) : (operand & 0xfffu);
    return (imm << 20) | (rs1 << 15) | (funct3[kind] << 12) | (rd << 7) | 0x13u;
}
static std::int64_t signed_value(std::uint32_t a) {
    return a < 0x80000000u ? std::int64_t(a) : std::int64_t(a) - 0x100000000ll;
}
static std::uint32_t reference(unsigned kind, std::uint32_t a, std::uint32_t b) {
    switch (kind) {
    case 0: return a & b;
    case 1: return a | b;
    case 2: return a ^ b;
    case 3: return a << (b % 32);
    case 4: return a >> (b % 32);
    case 5: {
        const auto scale = std::int64_t{1} << (b % 32);
        const auto value = signed_value(a);
        auto quotient = value / scale;
        if (value < 0 && value % scale) --quotient;
        return static_cast<std::uint32_t>(quotient);
    }
    case 6: return signed_value(a) < signed_value(b);
    case 7: return a < b;
    }
    throw std::runtime_error("invalid reference operation");
}
int main() {
    std::mt19937 random(1009);
    for (unsigned config = 0; config < 4; ++config) {
        const auto mode = config == 1 ? CPU::Mode::Microprogrammed : CPU::Mode::Hardwired;
        const auto execution = config >= 2 ? CPU::Execution::Pipelined : CPU::Execution::MultiCycle;
        for (unsigned kind = 0; kind < 8; ++kind) {
            for (unsigned sample = 0; sample < 64; ++sample) {
                const std::uint32_t edges[]{0, 1, 0x7fffffff, 0x80000000, 0xffffffff};
                const auto a = sample < 5 ? edges[sample] : random();
                const auto b = sample < 5 ? edges[4 - sample] : random();
                for (bool immediate : {false, true}) {
                    const int imm = sample < 5 ? (sample == 0 ? -2048 : sample == 1 ? 2047 : sample == 2 ? -1 : 0)
                                              : static_cast<int>(random() % 4096) - 2048;
                    auto operand = immediate ? static_cast<std::uint32_t>(imm) : b;
                    if (immediate && kind >= 3 && kind <= 5) operand = sample % 32;
                    CPU cpu({encode(kind, immediate, 3, 1, immediate ? operand : 2)}, mode, 16, {}, execution, {config != 3});
                    cpu.set_register(1, a); cpu.set_register(2, b); cpu.step_instruction(1000);
                    require(cpu.registers().read(3) == reference(kind, a, operand) && cpu.pc() == 4,
                            "integer instruction disagrees with independent reference");
                }
            }
        }
        // Consecutive producers/consumers exercise forwarding or interlocks, including masked shifts.
        CPU chain({0xfff00093, encode(4, true, 2, 1, 1), encode(3, false, 3, 2, 1),
                   encode(5, true, 4, 3, 31), encode(2, true, 6, 4, 0xffffffff),
                   encode(0, true, 7, 2, 255), encode(1, false, 8, 7, 3)}, mode, 16, {}, execution, {config != 3});
        chain.run_instructions(7, 1000);
        require(chain.registers().read(3) == 0x80000000 && chain.registers().read(4) == 0xffffffff &&
                chain.registers().read(6) == 0 && chain.registers().read(8) == 0x800000ff,
                "integer dependency chain failed");
        CPU zero({encode(0, true, 0, 0, 0xffffffff)}, mode, 16, {}, execution, {config != 3});
        zero.step_instruction(1000); require(zero.registers().read(0) == 0, "integer instruction changed x0");
    }
    for (const auto word : {0x02001013u, 0x02005013u, 0x40001013u, 0x4020f1b3u, 0x0420d1b3u}) {
        bool rejected = false;
        try { (void)colab::soc::core::Decoder{}.decode_rv32(word); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "reserved integer encoding accepted");
    }
    std::cout << "PASS RV32 bitwise/shift/compare instructions: independent reference, edges, hazards and reserved encodings\n";
}
