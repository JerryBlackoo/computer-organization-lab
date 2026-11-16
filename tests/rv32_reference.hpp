#pragma once

#include "rv32_test_support.hpp"
#include <array>

using namespace rv32_test;

// 指令级参照直接解析机器字、执行架构语义；不调用共享 Decoder、ALU 或 Datapath。
struct Reference {
    std::array<std::uint32_t, 32> registers{};
    std::array<std::uint32_t, 64> memory{};
    std::uint32_t pc = 0;
    static std::uint32_t extend(std::uint32_t value, unsigned width) {
        return (value & (1u << (width - 1))) ? value | (~0u << width) : value;
    }
    void step(std::uint32_t word) {
        const auto opcode = word & 127u;
        const auto rd = (word >> 7) & 31u, rs1 = (word >> 15) & 31u, rs2 = (word >> 20) & 31u;
        auto next_pc = pc + 4;
        switch (opcode) {
        case 0x33:
            registers[rd] = word & 0x40000000u ? registers[rs1] - registers[rs2] : registers[rs1] + registers[rs2];
            break;
        case 0x13: registers[rd] = registers[rs1] + extend(word >> 20, 12); break;
        case 0x03: {
            const auto address = registers[rs1] + extend(word >> 20, 12);
            assert(address % 4 == 0 && address / 4 < memory.size());
            registers[rd] = memory[address / 4];
            break;
        }
        case 0x23: {
            const auto immediate = extend(((word >> 25) << 5) | ((word >> 7) & 31u), 12);
            const auto address = registers[rs1] + immediate;
            assert(address % 4 == 0 && address / 4 < memory.size());
            memory[address / 4] = registers[rs2];
            break;
        }
        case 0x63: {
            const auto immediate = extend(((word >> 31) << 12) | (((word >> 7) & 1u) << 11)
                | (((word >> 25) & 63u) << 5) | (((word >> 8) & 15u) << 1), 13);
            if (registers[rs1] == registers[rs2]) next_pc = pc + immediate;
            break;
        }
        default: assert(false);
        }
        registers[0] = 0;
        pc = next_pc;
    }
};

void compare_to_reference(const CPU& cpu, const Reference& reference) {
    assert(cpu.pc() == reference.pc);
    for (unsigned reg = 0; reg < 32; ++reg) assert(cpu.registers().read(reg) == reference.registers[reg]);
    for (unsigned address = 0; address < 256; address += 4)
        assert(cpu.memory().read(address) == reference.memory[address / 4]);
}

