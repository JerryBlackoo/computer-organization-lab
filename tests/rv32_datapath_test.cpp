#include "../soc/rv32/datapath/dedicated/alu.hpp"
#include "../soc/rv32/datapath/dedicated/register_file.hpp"
#include "../soc/rv32/datapath/dedicated/memory.hpp"
#include <cassert>
#include <random>

using namespace colab::soc::core::datapath::dedicated;

int main() {
    ALU alu;
    assert(alu.evaluate(ALUCtr::Add, 0x7FFFFFFF, 1).overflow);
    assert(!alu.evaluate(ALUCtr::Addu, 0x7FFFFFFF, 1).overflow);
    assert(alu.evaluate(ALUCtr::Sub, 0x80000000, 1).overflow);
    assert(!alu.evaluate(ALUCtr::Subu, 0x80000000, 1).overflow);
    std::mt19937 random(0x32A1);
    for (unsigned n = 0; n < 10000; ++n) {
        const std::uint32_t a = random(), b = random();
        assert(alu.evaluate(ALUCtr::Addu, a, b).result == a + b);
        assert(alu.evaluate(ALUCtr::Subu, a, b).result == a - b);
        assert(alu.evaluate(ALUCtr::And, a, b).result == (a & b));
        assert(alu.evaluate(ALUCtr::Or, a, b).result == (a | b));
        assert(alu.evaluate(ALUCtr::Xor, a, b).result == (a ^ b));
        assert(alu.evaluate(ALUCtr::Nor, a, b).result == ~(a | b));
        assert(alu.evaluate(ALUCtr::Sltu, a, b).result == (a < b));
        const auto signed_a = static_cast<std::int64_t>(a) - (a >> 31 ? 0x100000000LL : 0);
        const auto signed_b = static_cast<std::int64_t>(b) - (b >> 31 ? 0x100000000LL : 0);
        assert(alu.evaluate(ALUCtr::Slt, a, b).result == (signed_a < signed_b));
    }
    RegisterFile registers;
    registers.write(0, 99); registers.write(31, 42);
    const auto ports = registers.read(0, 31);
    assert(ports.bus_a == 0 && ports.bus_b == 42);
    registers.write(31, 7, false); assert(registers.read(31) == 42);
    DataMemory memory(2);
    memory.write(4, 0xABCDEF01); assert(memory.read(4) == 0xABCDEF01);
    bool unaligned = false, outside = false;
    try { memory.read(2); } catch (const std::invalid_argument&) { unaligned = true; }
    try { memory.write(8, 1); } catch (const std::out_of_range&) { outside = true; }
    assert(unaligned && outside);
}
