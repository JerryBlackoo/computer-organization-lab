#pragma once

#include "rank/rank.hpp"

#include <vector>

namespace colab::dimm {

// 与 SoC 内存控制器的接线约定（供之后接入 MemoryController）：
//
// 系统地址总线 -> MemoryController 高位地址译码/片选 -> DIMM 局部地址
//             -> Rank 选择 -> Bank / 行 / 列译码。
// 本类的地址均从 DIMM 内部的 0 开始；系统物理地址须先由控制器判断是否命中
// 分配给本 DIMM 的地址窗口，命中后减去窗口基址，才交给本类。未选中时不发出访问。
// 不能直接截掉系统地址高位，否则其他设备的地址也会错误地映射到同一片内位置。
//
// 例：16 位总线地址、每个模块 4K 个可寻址单元，且双方的地址单位一致、窗口按 4K 对齐：
//   A15..A12 -> 高 4 位，经译码产生片选；A11..A0 -> 低 12 位，作为模块内地址。
//   若该模块映射在 0x8000..0x8FFF，则高 4 位等于 0x8 时选中，局部地址为 A & 0x0FFF。
// 这里的 4K 是地址单元数。若为 4K x 8 bit 且总线按字节编址，它才对应 4 KiB 窗口。
// 若 Rank 为 4K x 16 bit 而总线按字节编址，则占 8 KiB：对齐窗口用 A15..A13 选片，
// A12..A1 提供 12 位局部字地址，A0 选择字内字节。不能直接混用字地址和字节地址。
//
// 本类 decode()/select_rank() 实现窗口内的 Rank 选择，Rank 的各颗位扩展 DRAM chip 同时工作，
// 不用额外地址位逐颗选芯片。MemoryController 接真实命令接口时，再把选择结果转成
// DIMM/Rank 的使能及 Bank、行、列控制信号；行列地址由 RAS/CAS 分时送到复用引脚。
// “12 位完整逻辑地址可寻址 4K 单元”与“12 根行列复用引脚”需分别计算，见 geometry.hpp。
class DIMM {
public:
    explicit DIMM(DIMMGeometry geometry = {}) : geometry_(geometry) {
        geometry_.validate();
        ranks_.reserve(geometry_.ranks);
        for (std::size_t i = 0; i < geometry_.ranks; ++i) ranks_.emplace_back(geometry_.rank);
    }

    const DIMMGeometry& geometry() const { return geometry_; }
    const Rank& rank(std::size_t index) const { return ranks_.at(index); }
    void activate(std::size_t rank, std::size_t bank, std::size_t row) { ranks_.at(rank).activate(bank, row); }
    void precharge(std::size_t rank, std::size_t bank) { ranks_.at(rank).precharge(bank); }
    std::optional<std::size_t> active_row(std::size_t rank, std::size_t bank) const {
        return ranks_.at(rank).dram_chip(0).bank(bank).state().active_row;
    }

    // 输入为已完成外部片选、基址转换后的 DIMM 局部字地址。
    DIMMAddress decode(std::size_t word_address) const {
        const auto rank_index = select_rank(word_address);
        const auto part =
            ranks_[rank_index].dram_chip(0).decode(word_address % geometry_.rank.word_count());
        return {rank_index, part.bank, part.row, part.column};
    }

    std::size_t encode(DIMMAddress address) const {
        if (address.rank >= geometry_.ranks) throw std::out_of_range("rank outside DIMM");
        const auto local =
            ranks_[address.rank].dram_chip(0).encode({address.bank, address.row, address.column});
        return address.rank * geometry_.rank.word_count() + local;
    }

    // 局部字地址：每增加 1，跨过整个 Rank 的数据宽度。不同 Rank 占连续地址段（字扩展）。
    Word read(std::size_t word_address) {
        const auto selected = select_rank(word_address);
        return ranks_[selected].read(word_address % geometry_.rank.word_count());
    }
    // Debug snapshot: inspect storage cells without issuing PRE/ACT/READ or
    // changing row buffers and command statistics, even during a timed request.
    Word inspect(std::size_t word_address) const {
        const auto selected = select_rank(word_address);
        const auto local = word_address % geometry_.rank.word_count();
        const auto& rank = ranks_[selected];
        Word value = 0;
        for (std::size_t i = 0; i < geometry_.rank.dram_chips; ++i) {
            const auto& chip = rank.dram_chip(i);
            const auto address = chip.decode(local);
            const auto& bank = chip.bank(address.bank);
            for (std::size_t bit = 0; bit < geometry_.rank.dram_chip.bank.data_bits; ++bit) {
                if (bank.inspect_cell(address.row, address.column, bit).read())
                    value |= Word{1} << (i * geometry_.rank.dram_chip.bank.data_bits + bit);
            }
        }
        return value;
    }
    void write(std::size_t word_address, Word data) {
        const auto selected = select_rank(word_address);
        ranks_[selected].write(word_address % geometry_.rank.word_count(), data);
    }

    // 局部小端字节接口：控制器传入物理地址减窗口基址后的字节偏移。
    // 要求 Rank 位宽为 8 的倍数；字节写用功能级读-改-写实现。
    std::uint8_t read_byte(std::size_t byte_address) {
        const auto bytes = bytes_per_word();
        const auto shift = (byte_address % bytes) * 8;
        return static_cast<std::uint8_t>((read(byte_address / bytes) >> shift) & 0xFFu);
    }
    void write_byte(std::size_t byte_address, std::uint8_t data) {
        const auto bytes = bytes_per_word();
        const auto word_address = byte_address / bytes;
        const auto shift = (byte_address % bytes) * 8;
        const auto old = read(word_address);
        write(word_address, (old & ~(Word{0xFF} << shift)) | (Word{data} << shift));
    }

    bool all_precharged() const {
        for (const auto& item : ranks_) if (!item.all_precharged()) return false;
        return true;
    }
    void precharge_all() {
        for (auto& item : ranks_) item.precharge_all();
    }
    void refresh_next() {
        if (!all_precharged()) throw std::logic_error("DIMM REF requires all ranks precharged");
        for (auto& item : ranks_) item.refresh_next();
    }

private:
    // 高地址部分选择 Rank，低地址部分进入 Rank；每 Rank 字数为 2^n 时，等价于
    // 高位 word_address >> n 选 Rank、低 n 位作 Rank 内地址。此处不是系统总线片选。
    std::size_t select_rank(std::size_t word_address) const {
        if (word_address >= geometry_.word_count()) throw std::out_of_range("address outside DIMM");
        return word_address / geometry_.rank.word_count();
    }
    std::size_t bytes_per_word() const {
        if (geometry_.word_bits() % 8 != 0) {
            throw std::logic_error("byte access requires a rank width divisible by 8");
        }
        return geometry_.word_bits() / 8;
    }

    DIMMGeometry geometry_;
    std::vector<Rank> ranks_;
};

} // namespace colab::dimm
