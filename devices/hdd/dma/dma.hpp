#pragma once

#include "../types.hpp"
#include "../../common/dma_bus.hpp"

#include <algorithm>
#include <utility>

namespace colab::devices::hdd {

// System DMA uses bus transactions; DMA below retains the standalone mechanics demo.
using BusDMA = common::DmaEngine;

// 周期挪用：每一缓冲字发一次 BR，获得一个总线周期后释放；不是扇区突发 DMA。
// 仲裁策略：DMA 的待处理请求高于 CPU 新请求，不能抢占已经开始的 CPU 周期。
class DMA {
public:
    bool requested() const { return pending_; }
    bool owns_bus() const { return granted_; }
    Time remaining_ms() const { return granted_ ? remaining_ : 0; }
    const Bytes& data_register() const { return register_; }

private:
    friend class colab::devices::HDD;
    bool pending_ = false, granted_ = false;
    Time remaining_ = 0;
    std::size_t offset_ = 0, count_ = 0;
    Operation operation_ = Operation::Read;
    Bytes register_;

    void request(Operation operation, std::size_t offset, std::size_t count, Bytes data = {}) {
        if (pending_) throw std::logic_error("DMA data register is occupied");
        operation_ = operation;
        offset_ = offset;
        count_ = count;
        register_ = std::move(data);
        pending_ = true;
    }
    bool grant(BusSignals signals, Time cycle) {
        if (!pending_ || granted_ || signals.cpu_cycle_in_progress) return false;
        granted_ = true;
        remaining_ = cycle;
        return true;
    }
    void advance(Time elapsed) {
        if (granted_) remaining_ = std::max(0.0L, remaining_ - elapsed);
    }
    Bytes complete(Bytes& host_memory) {
        Bytes input;
        if (operation_ == Operation::Read) {
            std::copy(register_.begin(), register_.end(), host_memory.begin() + offset_);
        } else {
            input.assign(host_memory.begin() + offset_, host_memory.begin() + offset_ + count_);
        }
        cancel();
        return input;
    }
    void cancel() {
        pending_ = granted_ = false;
        remaining_ = 0;
        register_.clear();
    }
};

} // namespace colab::devices::hdd
