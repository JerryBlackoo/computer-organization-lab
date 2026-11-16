#include "package.hpp"

#include <iomanip>
#include <iostream>

int main() {
    using namespace colab::dimm;

    // 用小阵列看清楚行为；接口与默认的 2048 x 2048 x 4 Bank 相同。
    Bank bank({4, 8, 4});
    bank.ras(2); // 共享地址引脚先传行地址 2，RAS 锁存并激活。
    (void)bank.cas(3, false, true, 0xA); // 再传列地址 3；/WE=0 写入。
    const auto data = bank.cas(3); // /WE=1，/OE=0，读出。
    std::cout << "Bank: row=" << *bank.state().row_address
              << ", column=" << *bank.state().column_address
              << ", data=0x" << std::hex << unsigned{*data} << std::dec << '\n';
    std::cout << "OE disabled: " << (bank.cas(3, true, true) ? "driven" : "high-Z") << '\n';
    bank.precharge();
    bank.refresh_next();
    std::cout << "Refreshed row=" << *bank.state().last_refresh_row
              << ", next=" << bank.state().next_refresh_row << '\n';

    DIMMGeometry geometry;
    geometry.ranks = 4;
    geometry.rank.dram_chips = 4;
    geometry.rank.dram_chip.banks = 4;
    geometry.rank.dram_chip.bank = {4, 8, 4};
    DIMM memory(geometry);
    std::cout << "\nDIMM: " << geometry.word_count() << " words x " << geometry.word_bits()
              << " bits = " << geometry.capacity_bytes() << " bytes\n";
    std::cout << "word address -> rank, bank, row, column\n";
    for (std::size_t address = 0; address < 8; ++address) {
        const auto decoded = memory.decode(address);
        std::cout << std::setw(4) << address << " -> " << decoded.rank << ", " << decoded.bank
                  << ", " << decoded.row << ", " << decoded.column << '\n';
    }

    memory.write(5, 0xBEEF);
    std::cout << "\nRank word[5] = 0x" << std::hex << memory.read(5) << '\n';
    std::cout << "Chip lanes (low bits first): ";
    for (std::size_t i = 0; i < geometry.rank.dram_chips; ++i) {
        std::cout << "0x" << unsigned{memory.rank(0).dram_chip(i).bank(1).row_buffer()[1]} << ' ';
    }
    std::cout << std::dec << '\n';

    std::cout << "Same local address in all four Ranks: 0x" << std::hex << memory.read(5);
    for (std::size_t rank = 1; rank < geometry.ranks; ++rank) {
        const auto address = rank * geometry.rank.word_count() + 5;
        memory.write(address, 0x1230 + rank);
        std::cout << " / 0x" << memory.read(address);
    }
    std::cout << std::dec << '\n';

    memory.write_byte(10, 0xAA); // 字地址 5 对应字节地址 10、11，小端。
    std::cout << "After byte write: word[5] = 0x" << std::hex << memory.read(5) << std::dec << '\n';

    // Bank 0：连续同一行的两次访问只 ACT 一次；换行时 PRE + ACT。
    memory.write(0, 1);
    memory.write(4, 2);
    memory.write(32, 3);
    const auto& stats = memory.rank(0).dram_chip(0).bank(0).stats();
    std::cout << "Bank 0: ACT=" << stats.activations << ", PRE=" << stats.precharges << '\n';

    memory.precharge_all();
    for (std::size_t row = 0; row < geometry.rank.dram_chip.bank.rows; ++row) memory.refresh_next();
    std::cout << "Refresh scan: " << memory.rank(0).dram_chip(0).bank(0).stats().refreshes
              << " rows per bank; word[5] still 0x" << std::hex << memory.read(5) << '\n';
}
