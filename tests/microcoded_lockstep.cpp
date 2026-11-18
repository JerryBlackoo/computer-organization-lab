#include "../soc/rv32/core.hpp"
#include <iomanip>
#include <iostream>
#include <vector>
#include <string>
int main(int argc, char** argv) {
    const bool vertical = argc > 1 && std::string(argv[1]) == "vertical";
    using CPU = colab::soc::RV32Core;
    CPU cpu({0x00a00093, 0x01400113, 0x002081b3, 0x00302023, 0x00002203,
             0x00418463, 0x06300293, 0x401202b3, 0x00208463, 0x00700313},
            vertical ? CPU::Mode::VerticalMicroprogrammed : CPU::Mode::Microprogrammed, 128);
    for (int cycle = 0; cpu.completed_instructions() < 9 && cycle < 200; ++cycle) {
        const auto& s = cpu.state();
        const auto& u = cpu.control_unit();
        std::cout << "TRACE " << std::dec << s.pc << ' ' << *u.micro_address() << ' '
                  << std::hex << std::setfill('0') << std::setw(16)
                  << (vertical ? u.vertical().micro_ir_word() : u.microprogrammed().micro_ir_word());
        auto hex32 = [](std::uint32_t v) { std::cout << ' ' << std::setw(8) << v; };
        hex32(s.ir); hex32(s.mar); hex32(s.mdr); hex32(s.a); hex32(s.b); hex32(s.alu_out);
        for (unsigned i = 1; i <= 6; ++i) hex32(s.gprs.read(i));
        hex32(cpu.memory().read(0));
        std::cout << '\n';
        cpu.tick();
    }
    return cpu.completed_instructions() == 9 ? 0 : 1;
}
