#pragma once

#include "../bank/bank.hpp"

#include <vector>

namespace colab::dimm {

class DRAMChip {
public:
    explicit DRAMChip(DRAMChipGeometry geometry = {}) : geometry_(geometry) {
        geometry_.validate();
        banks_.reserve(geometry_.banks);
        for (std::size_t i = 0; i < geometry_.banks; ++i) banks_.emplace_back(geometry_.bank);
    }

    const DRAMChipGeometry& geometry() const { return geometry_; }
    Bank& bank(std::size_t index) { return banks_.at(index); }
    const Bank& bank(std::size_t index) const { return banks_.at(index); }

    // 低位多体交叉：相邻芯片字落到不同 Bank；Bank 内按行优先排列。
    // 输入已是片内地址，外部总线高位片选在 MemoryController 完成，接线约定见 dimm.hpp。
    DRAMChipAddress decode(std::size_t word_address) const {
        if (word_address >= geometry_.word_count()) throw std::out_of_range("address outside DRAM chip");
        const auto local = word_address / geometry_.banks;
        return {word_address % geometry_.banks,
                local / geometry_.bank.columns, local % geometry_.bank.columns};
    }

    std::size_t encode(DRAMChipAddress address) const {
        if (address.bank >= geometry_.banks || address.row >= geometry_.bank.rows ||
            address.column >= geometry_.bank.columns) {
            throw std::out_of_range("decoded address outside DRAM chip");
        }
        return (address.row * geometry_.bank.columns + address.column) * geometry_.banks + address.bank;
    }

    // 便捷读写执行最小的 PRE/ACT/READ 或 WRITE 命令序列，并保留打开的行。
    std::uint8_t read(std::size_t word_address) {
        const auto address = decode(word_address);
        open_row(address);
        return bank(address.bank).read(address.column);
    }

    void write(std::size_t word_address, Word data) {
        const auto address = decode(word_address);
        detail::check_value(data, geometry_.bank.data_bits);
        open_row(address);
        bank(address.bank).write(address.column, data);
    }

    bool all_precharged() const {
        for (const auto& item : banks_) if (!item.is_precharged()) return false;
        return true;
    }

    void precharge_all() {
        for (auto& item : banks_) item.precharge();
    }

    // 每个 Bank 刷新计数器指向的一行；调用 rows 次构成一次完整刷新扫描。
    void refresh_next() {
        if (!all_precharged()) throw std::logic_error("DRAM chip REF requires all banks precharged");
        for (auto& item : banks_) item.refresh_next();
    }

private:
    void open_row(DRAMChipAddress address) {
        auto& selected = bank(address.bank);
        if (selected.state().active_row == address.row) return; // 行命中，无需重新 ACT。
        selected.precharge();
        selected.activate(address.row);
    }

    DRAMChipGeometry geometry_;
    std::vector<Bank> banks_;
};

} // namespace colab::dimm