#pragma once

#include "../page_table_walker/page_table_walker.hpp"

#include <algorithm>
#include <vector>

namespace colab::soc::core::mmu {

// 物理字节存储到页表项的适配器；不认识 Cache、控制器或 DIMM 类型。
class PageTablePort {
public:
    using Bytes = std::vector<std::uint8_t>;
    using ReadBytes = std::function<Bytes(Address, std::size_t)>;
    using WriteBytes = std::function<void(Address, const Bytes&)>;

    PageTablePort(ReadBytes read, WriteBytes write) : read_(std::move(read)), write_(std::move(write)) {
        if (!read_ || !write_) throw std::invalid_argument("page table port requires physical read/write callbacks");
    }

    PageTableWalker::ReadEntry reader() const {
        return [read = read_](Address address) {
            const auto data = read(address, PageTableEntryCodec::byte_size);
            if (data.size() != PageTableEntryCodec::byte_size) throw std::runtime_error("incomplete page table entry");
            PageTableEntryCodec::Bytes bytes{};
            std::copy(data.begin(), data.end(), bytes.begin());
            return PageTableEntryCodec::decode(bytes);
        };
    }

    PageTableWalker::WriteEntry writer() const {
        return [write = write_](Address address, const PageTableEntry& entry) {
            const auto bytes = PageTableEntryCodec::encode(entry);
            write(address, Bytes(bytes.begin(), bytes.end()));
        };
    }

private:
    ReadBytes read_;
    WriteBytes write_;
};

} // namespace colab::soc::core::mmu
