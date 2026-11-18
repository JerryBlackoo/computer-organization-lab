#include "rv32_test_support.hpp"
#include "../soc/rv32/control_unit/microprogrammed/encoding.hpp"
#include <cassert>
#include <cstdint>

using namespace rv32_test;
using namespace colab::soc::core::control_unit::microprogrammed;

int main() {
    ControlStore rom;
    // Golden machine words are independent of the C++ layout/sizeof.
    assert(rom.raw_at(0) == UINT64_C(0x00200001));
    assert(rom.raw_at(16) == UINT64_C(0x00200020));
    assert(rom.raw_at(17) == UINT64_C(0x00200100));
    assert(rom.raw_at(18) == UINT64_C(0x80021c000));
    for (MicroAddress address = 0; address < 256; ++address) {
        if (rom.valid(address)) {
            const auto word = rom.raw_at(address);
            assert(encode(decode(word, rom.phase_at(address))) == word);
        }
    }
    assert(!rom.valid(63));
    expect_throw<std::invalid_argument>([&] { rom.set_raw(17, UINT64_C(1) << 63, rom.phase_at(17)); });
    expect_throw<std::invalid_argument>([&] { rom.set_raw(17, UINT64_C(3) << 18, rom.phase_at(17)); });
    expect_throw<std::invalid_argument>([&] { rom.set_raw(17, UINT64_C(7) << 35, rom.phase_at(17)); });
    assert(rom.raw_at(17) == UINT64_C(0x00200100));
    auto invalid = rom.at(18); invalid.next_address = 256;
    expect_throw<std::invalid_argument>([&] { rom.set(18, invalid); });

    // Patch the real binary control word, not the struct: ALU op Addu=1 -> Subu=3.
    rom.set_raw(17, (rom.raw_at(17) & ~(UINT64_C(31) << 21)) | (UINT64_C(3) << 21), rom.phase_at(17));
    CPU cpu({r(3, 1, 2)}, Mode::Microprogrammed, 16, rom);
    cpu.set_register(1, 20); cpu.set_register(2, 7);
    cpu.step_instruction();
    assert(cpu.registers().read(3) == 13);
}
