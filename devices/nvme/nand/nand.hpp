#pragma once

#include "../../common/mmio_device.hpp"
#include <map>

namespace colab::devices::nvme {
// Sparse namespace backing store. FTL/erase/endurance are outside this I/O model.
class NAND {
public:
    static constexpr std::size_t block_bytes = 512;
    explicit NAND(std::uint64_t blocks = 4096) : blocks_(blocks) {
        if (!blocks) throw std::invalid_argument("empty NAND namespace");
    }
    std::uint64_t blocks() const { return blocks_; }
    void check(std::uint64_t lba, std::size_t count) const {
        if (!count || lba >= blocks_ || count > blocks_ - lba) throw std::out_of_range("NAND LBA out of range");
    }
    common::Bytes read(std::uint64_t lba, std::size_t count) const {
        check(lba, count);
        common::Bytes result(count * block_bytes, 0);
        for (std::size_t i = 0; i < count; ++i) {
            const auto found = data_.find(lba + i);
            if (found != data_.end()) std::copy(found->second.begin(), found->second.end(), result.begin() + i * block_bytes);
        }
        return result;
    }
    void write(std::uint64_t lba, const common::Bytes& bytes) {
        if (bytes.empty() || bytes.size() % block_bytes) throw std::invalid_argument("NAND requires whole blocks");
        check(lba, bytes.size() / block_bytes);
        for (std::size_t i = 0; i < bytes.size() / block_bytes; ++i)
            data_[lba + i] = common::Bytes(bytes.begin() + i * block_bytes, bytes.begin() + (i + 1) * block_bytes);
    }
private:
    std::uint64_t blocks_;
    std::map<std::uint64_t, common::Bytes> data_;
};
}
