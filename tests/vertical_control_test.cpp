#include "rv32_test_support.hpp"
#include "../soc/rv32/control_unit/vertical/vertical_controller.hpp"
#include <cassert>
#include <vector>

using namespace rv32_test;
namespace v = colab::soc::core::control_unit::vertical;
namespace m = colab::soc::core::control_unit::microprogrammed;
int main() {
    m::ControlStore rom;
    v::Controller vertical(rom);
    for (m::MicroAddress address = 0; address < 256; ++address) {
        if (!rom.valid(address)) continue;
        const auto word = vertical.at(address);
        assert((word >> v::word_bits) == 0);
        assert(m::encode(v::decode(word, rom.phase_at(address))) == rom.raw_at(address));
    }
    assert((vertical.at(16) & 31) == static_cast<unsigned>(v::Op::ReadOperands));
    assert((vertical.at(17) & 31) == static_cast<unsigned>(v::Op::Alu));
    assert((vertical.at(18) & 31) == static_cast<unsigned>(v::Op::WriteRegister));
    expect_throw<std::invalid_argument>([] { v::decode(31, Phase::Execute); });
    expect_throw<std::invalid_argument>([] { v::decode(std::uint64_t{1} << 47, Phase::Execute); });

    const std::vector<std::uint32_t> program{i(1,0,10), i(2,0,20), r(3,1,2), sw(3,0,4),
        i(4,0,4,true), beq(3,4,8), i(5,0,99), r(5,4,1,true)};
    CPU horizontal(program, Mode::Microprogrammed, 16), encoded(program, Mode::VerticalMicroprogrammed, 16);
    while (horizontal.completed_instructions() < 7) {
        assert(horizontal.pc() == encoded.pc());
        assert(horizontal.control_unit().microprogrammed().micro_pc() == encoded.control_unit().vertical().micro_pc());
        assert(m::encode(v::decode(encoded.control_unit().vertical().micro_ir_word(), encoded.control_unit().rv32_phase()))
               == horizontal.control_unit().microprogrammed().micro_ir_word());
        horizontal.tick(); encoded.tick();
        compare_architecture(horizontal, encoded);
        assert(horizontal.state().a == encoded.state().a && horizontal.state().b == encoded.state().b);
        assert(horizontal.state().alu_out == encoded.state().alu_out);
        assert(encoded.last_cycle()->micro_address.has_value());
    }
    assert(encoded.registers().read(5) == 20);
    CPU switching({i(1,0,1),i(2,1,1),i(3,2,1)});
    switching.step_instruction(); switching.select_mode(Mode::VerticalMicroprogrammed);
    switching.tick(); expect_throw<std::logic_error>([&] { switching.select_mode(Mode::Microprogrammed); });
    switching.step_instruction(); switching.select_mode(Mode::Microprogrammed); switching.step_instruction();
    assert(switching.registers().read(3) == 3);
    // Unsupported custom control combinations fail when selecting vertical mode,
    // without preventing the existing hardwired or horizontal controller from running.
    auto patched = rom.at(16); patched.control.alu_in = true; rom.set(16, patched);
    CPU custom({i(1,0,1)}, Mode::Hardwired, 16, rom);
    expect_throw<std::invalid_argument>([&] { custom.select_mode(Mode::VerticalMicroprogrammed); });
    assert(custom.mode() == Mode::Hardwired);
    custom.step_instruction(); assert(custom.registers().read(1) == 1);
}
