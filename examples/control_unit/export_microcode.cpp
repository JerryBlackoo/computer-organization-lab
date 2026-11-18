#include "../../soc/rv32/control_unit/vertical/vertical_controller.hpp"
#include <string>
#include <iomanip>
#include <iostream>

// readmemh image: 1 valid bit followed by the 64-bit microinstruction.
int main(int argc, char** argv) {
    const bool vertical = argc == 2 && std::string(argv[1]) == "vertical";
    if (argc > 2 || (argc == 2 && !vertical)) return 2;
    using namespace colab::soc::core::control_unit::microprogrammed;
    const ControlStore rom;
    for (MicroAddress address = 0; address < ControlStore::capacity; ++address) {
        const bool valid = rom.valid(address);
        std::cout << (valid ? '1' : '0') << std::hex << std::setfill('0') << std::setw(16)
                  << (valid ? (vertical ? colab::soc::core::control_unit::vertical::assemble(rom.raw_at(address)) : rom.raw_at(address)) : 0) << '\n';
    }
}
