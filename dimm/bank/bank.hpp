#pragma once

#include "../cell/cell.hpp"
#include "../geometry.hpp"

#include <algorithm>
#include <optional>
#include <vector>

namespace colab::dimm {

enum class RowSource { AddressPins, RefreshCounter };

struct BankState {
    std::size_t address_pins = 0; // 同一组地址引脚：RAS 时送行号，CAS 时送列号。
    std::optional<std::size_t> row_address; // 行地址锁存器
    std::optional<std::size_t> column_address; // 列地址锁存器
    std::optional<std::size_t> active_row;
    RowSource row_source = RowSource::AddressPins; // 行译码器前的 MUX
    std::optional<std::size_t> decoded_row;
    std::optional<std::uint8_t> input_buffer;
    std::optional<std::uint8_t> output_buffer;
    std::optional<std::size_t> last_refresh_row;
    std::size_t next_refresh_row = 0;
};

struct BankStats {
    std::size_t activations = 0;
    std::size_t reads = 0;
    std::size_t writes = 0;
    std::size_t precharges = 0;
    std::size_t refreshes = 0;
};

// 功能级 Bank：命令立即完成；不在这里模拟模拟电路或 DRAM 周期约束。
class Bank {
public:
    explicit Bank(BankGeometry geometry = {}) : geometry_(geometry) {
        geometry_.validate();
        cells_.resize(geometry_.word_count(), 0);
        row_buffer_.resize(geometry_.columns, 0);
    }

    const BankGeometry& geometry() const { return geometry_; }
    const BankState& state() const { return state_; }
    const BankStats& stats() const { return stats_; }
    const std::vector<std::uint8_t>& row_buffer() const { return row_buffer_; }
    bool is_precharged() const { return !state_.active_row.has_value(); }

    // 行译码 + 感应放大：将一整行送入行缓冲器；已有打开的行时必须先 PRE。
    void activate(std::size_t row) {
        check_row(row);
        require_precharged();
        const auto start = row * geometry_.columns;
        std::copy_n(cells_.begin() + start, geometry_.columns, row_buffer_.begin());
        state_.address_pins = row;
        state_.row_address = row;
        state_.column_address.reset();
        state_.active_row = row;
        state_.row_source = RowSource::AddressPins;
        state_.decoded_row = row;
        state_.output_buffer.reset();
        ++stats_.activations;
    }

    std::uint8_t read(std::size_t column) {
        require_active();
        check_column(column);
        state_.address_pins = column;
        state_.column_address = column;
        state_.output_buffer = row_buffer_[column];
        ++stats_.reads;
        return *state_.output_buffer;
    }

    void write(std::size_t column, Word data) {
        require_active();
        check_column(column);
        detail::check_value(data, geometry_.data_bits);
        const auto lane = static_cast<std::uint8_t>(data);
        state_.address_pins = column;
        state_.column_address = column;
        state_.input_buffer = lane;
        state_.output_buffer.reset();
        row_buffer_[column] = lane;
        // 行打开时阵列与感应放大器相连，写操作同时更新阵列，PRE 不充当缓存写回。
        cells_[*state_.active_row * geometry_.columns + column] = lane;
        ++stats_.writes;
    }

    // 教材式地址复用接口：一次调用表示一次选通事件，并非逐电平/逐边沿仿真。
    void ras(std::size_t address) { activate(address); }

    // CAS 锁存列号。/WE=0 写；/WE=1 读；/OE=1 时输出端为高阻，用 nullopt 表示。
    std::optional<std::uint8_t> cas(std::size_t address, bool we_n = true,
                                    bool oe_n = false, Word data = 0) {
        if (!we_n) {
            write(address, data);
            return std::nullopt;
        }
        const auto value = read(address);
        return oe_n ? std::nullopt : std::optional<std::uint8_t>{value};
    }

    void precharge() {
        if (is_precharged()) return;
        state_.active_row.reset();
        state_.output_buffer.reset();
        std::fill(row_buffer_.begin(), row_buffer_.end(), 0);
        ++stats_.precharges;
    }

    // 刷新选择内部行号，不改外部地址引脚/行地址锁存器。
    // 数字模型没有电荷泄漏，数据保持不变；记录被恢复的行和刷新次数。
    void refresh(std::size_t row) {
        check_row(row);
        require_precharged();
        state_.row_source = RowSource::RefreshCounter;
        state_.decoded_row = row;
        state_.last_refresh_row = row;
        ++stats_.refreshes;
    }

    void refresh_next() {
        refresh(state_.next_refresh_row);
        state_.next_refresh_row = (state_.next_refresh_row + 1) % geometry_.rows;
    }

    // 调试观察接口，不发出读命令；返回一个 Cell 的值快照，修改快照不会写入 Bank。
    Cell inspect_cell(std::size_t row, std::size_t column, std::size_t plane) const {
        check_row(row);
        check_column(column);
        if (plane >= geometry_.data_bits) throw std::out_of_range("bit plane outside bank");
        return Cell{((cells_[row * geometry_.columns + column] >> plane) & 1u) != 0};
    }

private:
    void check_row(std::size_t row) const {
        if (row >= geometry_.rows) throw std::out_of_range("row outside bank");
    }
    void check_column(std::size_t column) const {
        if (column >= geometry_.columns) throw std::out_of_range("column outside bank");
    }
    void require_active() const {
        if (is_precharged()) throw std::logic_error("READ/WRITE requires an active row");
    }
    void require_precharged() const {
        if (!is_precharged()) throw std::logic_error("ACT/REF requires a precharged bank");
    }

    BankGeometry geometry_;
    std::vector<std::uint8_t> cells_; // index = row * columns + column，低 data_bits 位有效。
    std::vector<std::uint8_t> row_buffer_; // 感应放大器/行缓冲器的逻辑模型
    BankState state_{};
    BankStats stats_{};
};

} // namespace colab::dimm
