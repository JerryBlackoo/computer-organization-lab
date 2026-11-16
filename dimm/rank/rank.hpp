#pragma once

#include "../dram_chip/dram_chip.hpp"

#include <vector>

namespace colab::dimm {

class Rank {
public:
    explicit Rank(RankGeometry geometry = {}) : geometry_(geometry) {
        geometry_.validate();
        dram_chips_.reserve(geometry_.dram_chips);
        for (std::size_t i = 0; i < geometry_.dram_chips; ++i) {
            dram_chips_.emplace_back(geometry_.dram_chip);
        }
    }

    const RankGeometry& geometry() const { return geometry_; }
    // 只读观察：Rank 发出的地址/命令由所有 DRAM chip 共同接收。
    const DRAMChip& dram_chip(std::size_t index) const { return dram_chips_.at(index); }

    Word read(std::size_t word_address) {
        Word value = 0;
        for (std::size_t i = 0; i < dram_chips_.size(); ++i) {
            value |= Word{dram_chips_[i].read(word_address)} << (i * geometry_.dram_chip.bank.data_bits);
        }
        return value;
    }

    void write(std::size_t word_address, Word data) {
        // 先验证，再广播，避免非法数据/地址造成部分芯片已写入。
        (void)dram_chips_.front().decode(word_address);
        detail::check_value(data, geometry_.word_bits());
        const auto chip_mask = detail::mask(geometry_.dram_chip.bank.data_bits);
        for (std::size_t i = 0; i < dram_chips_.size(); ++i) {
            const auto part =
                static_cast<std::uint8_t>((data >> (i * geometry_.dram_chip.bank.data_bits)) & chip_mask);
            dram_chips_[i].write(word_address, part);
        }
    }

    bool all_precharged() const {
        for (const auto& item : dram_chips_) if (!item.all_precharged()) return false;
        return true;
    }
    void precharge_all() {
        for (auto& item : dram_chips_) item.precharge_all();
    }
    void refresh_next() {
        if (!all_precharged()) throw std::logic_error("rank REF requires all DRAM chips precharged");
        for (auto& item : dram_chips_) item.refresh_next();
    }

private:
    RankGeometry geometry_;
    std::vector<DRAMChip> dram_chips_; // 位扩展：chip 0 提供最低 data_bits 位。
};

} // namespace colab::dimm