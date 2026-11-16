#pragma once

#include "../types.hpp"

#include <deque>
#include <stdexcept>
#include <utility>

namespace colab::soc::cache {

struct PendingWrite { Address address; Bytes data; };

// FIFO 写缓冲。请求拥有数据副本；下级写失败时保留队首，允许宿主重试。
class WriteBuffer {
public:
    explicit WriteBuffer(std::size_t capacity) : capacity_(capacity) {}
    bool full() const { return pending_.size() == capacity_; }
    std::size_t size() const { return pending_.size(); }
    std::size_t capacity() const { return capacity_; }

    void enqueue(Address address, Bytes data) {
        if (full()) throw std::logic_error("write buffer is full");
        pending_.push_back({address, std::move(data)});
    }

    bool drain_one(const WriteMemory& writer) {
        if (pending_.empty()) return false;
        writer(pending_.front().address, pending_.front().data);
        pending_.pop_front();
        return true;
    }

private:
    std::size_t capacity_;
    std::deque<PendingWrite> pending_;
};

} // namespace colab::soc::cache
