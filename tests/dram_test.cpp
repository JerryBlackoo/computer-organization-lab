#include "../dimm/package.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace colab::dimm;

template <typename Exception, typename Function>
void expect_throw(Function function) {
    bool threw = false;
    try { function(); }
    catch (const Exception&) { threw = true; }
    assert(threw);
}

void test_bank() {
    Cell cell;
    assert(!cell.read());
    cell.write(true);
    assert(cell.read());

    const BankGeometry textbook;
    assert(textbook.word_count() == 4 * 1024 * 1024);
    assert(textbook.capacity_bytes() == 2 * 1024 * 1024);
    assert(textbook.address_pin_count() == 11);

    Bank bank({3, 5, 4}); // 非 2 的幂也能正确检查行列范围。
    assert(bank.geometry().address_pin_count() == 3);
    assert(bank.is_precharged());
    expect_throw<std::logic_error>([&] { bank.read(0); });
    expect_throw<std::logic_error>([&] { bank.write(0, 0); });
    bank.ras(1);
    assert(bank.state().address_pins == 1);
    assert(bank.state().row_address == 1);
    assert(bank.state().row_source == RowSource::AddressPins);
    assert(bank.state().decoded_row == 1);
    assert(bank.row_buffer().size() == 5);
    for (const auto value : bank.row_buffer()) assert(value == 0);
    expect_throw<std::logic_error>([&] { bank.activate(1); });
    expect_throw<std::logic_error>([&] { bank.activate(2); });
    expect_throw<std::logic_error>([&] { bank.refresh_next(); });
    assert(bank.state().next_refresh_row == 0);
    assert(bank.stats().refreshes == 0);

    assert(!bank.cas(4, false, true, 0xB)); // /OE 不阻止写入；写操作不驱动输出。
    assert(bank.state().address_pins == 4);
    assert(bank.state().row_address == 1); // 列选通没有覆盖行地址锁存器。
    assert(bank.state().column_address == 4);
    assert(bank.state().input_buffer == 0xB);
    assert(!bank.state().output_buffer);
    assert(bank.row_buffer()[4] == 0xB);
    assert(bank.inspect_cell(1, 4, 0).read());
    assert(bank.inspect_cell(1, 4, 1).read());
    assert(!bank.inspect_cell(1, 4, 2).read());
    assert(bank.inspect_cell(1, 4, 3).read());
    auto snapshot = bank.inspect_cell(1, 4, 0);
    snapshot.write(false);
    assert(bank.inspect_cell(1, 4, 0).read());
    assert(bank.cas(4).value() == 0xB);
    assert(!bank.cas(4, true, true)); // 高阻与读到数据 0 是不同的结果。
    assert(bank.state().output_buffer == 0xB);
    assert(bank.cas(0).has_value() && bank.cas(0).value() == 0);
    expect_throw<std::out_of_range>([&] { bank.write(4, 0x10); });
    expect_throw<std::out_of_range>([&] { bank.write(4, Word{1} << 40); });
    expect_throw<std::out_of_range>([&] { bank.cas(4, false, false, 0x100); });
    expect_throw<std::out_of_range>([&] { bank.read(5); });
    expect_throw<std::out_of_range>([&] { bank.write(5, 0); });
    assert(bank.read(4) == 0xB);

    bank.precharge();
    bank.precharge(); // 已关闭时 PRE 幂等。
    assert(bank.stats().precharges == 1);
    assert(!bank.state().output_buffer);
    expect_throw<std::out_of_range>([&] { bank.activate(3); });
    bank.activate(2);
    assert(bank.read(4) == 0); // 行之间独立。
    bank.write(4, 7);
    bank.precharge();
    bank.activate(1);
    assert(bank.read(4) == 0xB);
    bank.precharge();

    const auto old_pins = bank.state().address_pins;
    const auto old_row_latch = bank.state().row_address;
    expect_throw<std::out_of_range>([&] { bank.refresh(3); });
    for (std::size_t i = 0; i < 7; ++i) {
        bank.refresh_next();
        assert(bank.state().last_refresh_row == i % 3);
        assert(bank.state().next_refresh_row == (i + 1) % 3);
        assert(bank.state().row_source == RowSource::RefreshCounter);
        assert(bank.is_precharged());
    }
    assert(bank.stats().refreshes == 7);
    assert(bank.state().address_pins == old_pins);
    assert(bank.state().row_address == old_row_latch);
    bank.activate(2);
    assert(bank.read(4) == 7); // 刷新不清零。
    expect_throw<std::out_of_range>([&] { bank.inspect_cell(0, 0, 4); });
    expect_throw<std::out_of_range>([&] { bank.inspect_cell(3, 0, 0); });
    expect_throw<std::out_of_range>([&] { bank.inspect_cell(0, 5, 0); });
}

void test_dram_chip() {
    DRAMChip dram_chip({4, {2, 3, 4}});
    // 连续的地址 0..3 必须落在四个不同 Bank，且行列地址相同。
    for (std::size_t i = 0; i < 4; ++i) {
        const auto address = dram_chip.decode(i);
        assert(address.bank == i && address.row == 0 && address.column == 0);
        dram_chip.write(i, static_cast<std::uint8_t>(i + 1));
    }
    for (std::size_t i = 0; i < 4; ++i) {
        assert(dram_chip.bank(i).state().active_row == 0);
        assert(dram_chip.read(i) == i + 1);
    }
    dram_chip.read(4); // Bank 0，同一行下一列。
    assert(dram_chip.bank(0).stats().activations == 1);
    assert(dram_chip.bank(0).stats().precharges == 0);
    dram_chip.read(12); // Bank 0，换到第 1 行。
    assert(dram_chip.bank(0).stats().activations == 2);
    assert(dram_chip.bank(0).stats().precharges == 1);
    assert(dram_chip.bank(1).stats().activations == 1); // 其余 Bank 保持打开。
    assert(dram_chip.bank(1).state().active_row == 0);
    assert(dram_chip.read(0) == 1);

    const auto activations = dram_chip.bank(0).stats().activations;
    expect_throw<std::out_of_range>([&] { dram_chip.write(12, 16); });
    expect_throw<std::out_of_range>([&] { dram_chip.write(12, 256); });
    assert(dram_chip.bank(0).stats().activations == activations);
    expect_throw<std::out_of_range>([&] { dram_chip.read(24); });
    expect_throw<std::out_of_range>([&] { dram_chip.write(24, 0); });
    expect_throw<std::out_of_range>([&] { dram_chip.bank(4); });
    expect_throw<std::out_of_range>([&] { dram_chip.encode({0, 0, 3}); });

    expect_throw<std::logic_error>([&] { dram_chip.refresh_next(); });
    for (std::size_t i = 0; i < 4; ++i) assert(dram_chip.bank(i).stats().refreshes == 0);
    dram_chip.precharge_all();
    dram_chip.refresh_next();
    for (std::size_t i = 0; i < 4; ++i) assert(dram_chip.bank(i).stats().refreshes == 1);
    assert(dram_chip.read(0) == 1);
}

void test_dimm() {
    const DIMMGeometry geometry{2, {4, {4, {2, 3, 4}}}};
    DIMM memory(geometry);
    assert(geometry.word_bits() == 16);
    assert(geometry.word_count() == 48);
    assert(geometry.capacity_bytes() == 96);
    const auto rank1 = memory.decode(24);
    assert(rank1.rank == 1 && rank1.bank == 0 && rank1.row == 0 && rank1.column == 0);

    memory.write(0, 0xABCD);
    memory.write(24, 0x1234);
    assert(memory.read(0) == 0xABCD);
    assert(memory.read(24) == 0x1234);
    const std::uint8_t lanes[] = {0xD, 0xC, 0xB, 0xA};
    for (std::size_t i = 0; i < 4; ++i) {
        const auto& bank = memory.rank(0).dram_chip(i).bank(0);
        assert(bank.row_buffer()[0] == lanes[i]);
        assert(bank.state().row_address == 0 && bank.state().column_address == 0);
    }
    assert(memory.read_byte(0) == 0xCD);
    assert(memory.read_byte(1) == 0xAB);
    memory.write_byte(1, 0x55);
    assert(memory.read(0) == 0x55CD);
    memory.write_byte(0, 0x66);
    assert(memory.read(0) == 0x5566);
    memory.write_byte(95, 0xAA); // 最后一个字节，且不破坏相邻字节。
    assert(memory.read(47) == 0xAA00);

    expect_throw<std::out_of_range>([&] { memory.write(0, 0x10000); });
    assert(memory.read(0) == 0x5566);
    expect_throw<std::out_of_range>([&] { memory.read(48); });
    expect_throw<std::out_of_range>([&] { memory.write(48, 0); });
    expect_throw<std::out_of_range>([&] { memory.read_byte(96); });
    expect_throw<std::out_of_range>([&] { memory.write_byte(96, 0); });
    expect_throw<std::out_of_range>([&] { memory.encode({2, 0, 0, 0}); });
    expect_throw<std::out_of_range>([&] { memory.encode({0, 4, 0, 0}); });
    expect_throw<std::out_of_range>([&] { memory.encode({0, 0, 2, 0}); });

    expect_throw<std::logic_error>([&] { memory.refresh_next(); });
    assert(memory.rank(0).dram_chip(0).bank(0).stats().refreshes == 0);
    memory.precharge_all();
    memory.refresh_next();
    memory.refresh_next();
    for (std::size_t rank = 0; rank < 2; ++rank) {
        for (std::size_t dram_chip_index = 0; dram_chip_index < 4; ++dram_chip_index) {
            for (std::size_t bank = 0; bank < 4; ++bank) {
                const auto& item = memory.rank(rank).dram_chip(dram_chip_index).bank(bank);
                assert(item.stats().refreshes == 2);
                assert(item.state().next_refresh_row == 0);
            }
        }
    }
    assert(memory.read(0) == 0x5566 && memory.read(24) == 0x1234);

    // 顶层刷新先验证所有 Rank，后执行，不能遇到后一个打开的 Rank 才报错。
    memory.precharge_all();
    memory.read(24);
    expect_throw<std::logic_error>([&] { memory.refresh_next(); });
    assert(memory.rank(0).dram_chip(0).bank(0).stats().refreshes == 2);
}

void test_against_flat_memory() {
    // 独立的平坦数组作参照；覆盖所有地址、多种 Bank 数和 Rank/Chip 位宽。
    for (const auto bank_count : {std::size_t{1}, std::size_t{3}, std::size_t{4}}) {
        for (const auto bits : {std::size_t{1}, std::size_t{4}, std::size_t{8}}) {
            DIMMGeometry geometry{3, {3, {bank_count, {3, 5, bits}}}};
            DIMM memory(geometry);
            const auto word_count = geometry.word_count();
            const auto value_mask = (Word{1} << geometry.word_bits()) - 1;
            std::vector<Word> reference(word_count);
            std::vector<bool> seen(word_count, false);
            for (std::size_t address = 0; address < word_count; ++address) {
                const auto decoded = memory.decode(address);
                const auto flat = ((decoded.rank * 3 + decoded.row) * 5 + decoded.column) * bank_count + decoded.bank;
                assert(flat < word_count && !seen[flat]); // 每个物理位置恰好对应一个地址。
                seen[flat] = true;
                assert(memory.encode(decoded) == address);
                assert(memory.read(address) == 0);
                reference[address] = (address * Word{0x9E3779B1} + 0xABCDEFull) & value_mask;
                memory.write(address, reference[address]);
            }
            for (std::size_t address = word_count; address-- > 0;) {
                assert(memory.read(address) == reference[address]);
            }
            // 确定性伪随机访问产生行冲突，与平坦数组逐次比较。
            std::uint32_t state = 42;
            for (std::size_t i = 0; i < 2000; ++i) {
                state = state * 1664525u + 1013904223u;
                const auto address = state % word_count;
                if (i % 3 == 0) {
                    reference[address] = (Word{state} << 7) & value_mask;
                    memory.write(address, reference[address]);
                } else {
                    assert(memory.read(address) == reference[address]);
                }
            }
            memory.precharge_all();
            for (std::size_t row = 0; row < 3; ++row) memory.refresh_next();
            for (std::size_t address = 0; address < word_count; ++address) {
                assert(memory.read(address) == reference[address]);
            }
        }
    }
}

void test_boundaries() {
    // 64 位组合避免移位 64 位，以及高字节读改写时的符号/截断错误。
    DIMM wide({2, {8, {2, {2, 2, 8}}}});
    const auto all_ones = std::numeric_limits<Word>::max();
    wide.write(0, all_ones);
    assert(wide.read(0) == all_ones);
    wide.write(1, 0xFEDCBA9876543210ull);
    assert(wide.read(1) == 0xFEDCBA9876543210ull);
    assert(wide.read_byte(15) == 0xFE);
    wide.write_byte(15, 0x80);
    assert(wide.read(1) == 0x80DCBA9876543210ull);
    wide.write(15, all_ones);
    assert(wide.read(15) == all_ones);

    DIMM narrow({1, {1, {1, {1, 1, 1}}}});
    narrow.write(0, 1);
    assert(narrow.read(0) == 1);
    expect_throw<std::out_of_range>([&] { narrow.write(0, 2); });
    expect_throw<std::logic_error>([&] { narrow.read_byte(0); });
    expect_throw<std::logic_error>([&] { narrow.write_byte(0, 0); });

    expect_throw<std::invalid_argument>([] { Bank invalid({0, 1, 4}); });
    expect_throw<std::invalid_argument>([] { Bank invalid({1, 0, 4}); });
    expect_throw<std::invalid_argument>([] { Bank invalid({1, 1, 0}); });
    expect_throw<std::invalid_argument>([] { Bank invalid({1, 1, 9}); });
    expect_throw<std::invalid_argument>([] { DRAMChip invalid({0, {1, 1, 4}}); });
    expect_throw<std::invalid_argument>([] { Rank invalid({0, {1, {1, 1, 4}}}); });
    expect_throw<std::invalid_argument>([] { Rank invalid({17, {1, {1, 1, 4}}}); });
    expect_throw<std::invalid_argument>([] { DIMM invalid({0, {1, {1, {1, 1, 4}}}}); });
    expect_throw<std::overflow_error>([] {
        Bank invalid({std::numeric_limits<std::size_t>::max(), 2, 1});
    });
    expect_throw<std::overflow_error>([] {
        DIMM invalid({std::numeric_limits<std::size_t>::max(), {1, {1, {1, 2, 1}}}});
    });

    // 默认层级容量：4M x 4 bit 芯片，四颗组成 16 位 Rank，四个 Rank 共 32 MiB。
    DIMM defaults;
    assert(defaults.geometry().ranks == 4);
    assert(defaults.geometry().capacity_bytes() == 32 * 1024 * 1024);
    const auto last = defaults.geometry().word_count() - 1;
    defaults.write(last, 0xCAFE);
    assert(defaults.read(last) == 0xCAFE);
    const auto decoded = defaults.decode(last);
    assert(decoded.rank == 3 && decoded.bank == 0 && decoded.row == 2047 && decoded.column == 2047);
}

int main() {
    test_bank();
    test_dram_chip();
    test_dimm();
    test_against_flat_memory();
    test_boundaries();
    std::cout << "DIMM tests passed\n";
}
