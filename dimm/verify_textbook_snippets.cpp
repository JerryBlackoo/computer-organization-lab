// One-off textbook alignment checks; not part of CI.
#include "package.hpp"
#include <iostream>

int main() {
    using namespace colab::dimm;

    // 3.2-05: 四体低位交叉，体号 = 地址 mod 4（与 DRAMChip::decode 的 bank 字段一致）。
    const unsigned seq[] = {3, 9, 17, 2, 51, 37, 13, 4, 8, 41, 67, 10};
    std::cout << "3.2-05 module/bank index (addr % 4):\n";
    for (unsigned a : seq) {
        std::cout << "  addr " << a << " -> " << (a % 4);
        if (a < 4) {
            DRAMChip dram_chip({4, {1, 1, 4}});
            std::cout << " (dram_chip.decode bank=" << dram_chip.decode(a).bank << ")";
        }
        std::cout << "\n";
    }

    // 3.3-01 scaled: 512Kx8 -> 4Mx32 is 32 chips; use 4 words x 32 from 1 word x 8, 4 chips, 2 ranks.
    DIMMGeometry g;
    g.ranks = 2;
    g.rank.dram_chips = 4;
    g.rank.dram_chip.banks = 1;
    g.rank.dram_chip.bank = {2, 2, 8}; // 4 words/chip x 8 bit
    DIMM mem(g);
    std::cout << "\n3.3-01 scaled: dram_chips=" << (g.ranks * g.rank.dram_chips)
              << " words=" << g.word_count() << " width=" << g.word_bits()
              << " bytes=" << g.capacity_bytes() << "\n";
    mem.write(5, 0xDEADBEEF);
    std::cout << "  word[5]=0x" << std::hex << mem.read(5) << std::dec << " decode rank="
              << mem.decode(5).rank << "\n";

    BankGeometry bg{2048, 2048, 4};
    std::cout << "\nRow/col mux pins (2048x2048): " << bg.address_pin_count()
              << " (22 bit row/col logic via RAS then CAS)\n";

    return 0;
}