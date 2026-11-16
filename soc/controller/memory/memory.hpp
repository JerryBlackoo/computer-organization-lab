#pragma once

#include "../../../dimm/dimm.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

namespace colab::soc::controller {

// 系统物理字节地址 -> DIMM 窗口片选 -> 局部字/字节 -> Rank/Bank/行/列。
// 这是同步功能接口；PRE/ACT/READ/WRITE 顺序由现有 DIMM 下层模型执行。
class Memory {
public:
    using Address = std::uint32_t;
    using Bytes = std::vector<std::uint8_t>;

    struct Statistics {
        std::uint64_t read_requests = 0, write_requests = 0;
        std::uint64_t bytes_read = 0, bytes_written = 0;
        std::uint64_t word_reads = 0, word_writes = 0;
    };

    struct DecodedAddress {
        std::size_t local_byte_address;
        std::size_t word_address;
        std::size_t byte_in_word;
        dimm::DIMMAddress dram;
    };

    Memory() = default;
    explicit Memory(dimm::DIMM& module, Address base = 0) { map(base, module); }
    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;
    Memory(Memory&&) = delete;
    Memory& operator=(Memory&&) = delete;

    void map(Address base, dimm::DIMM& module) {
        if (module.geometry().word_bits() % 8 != 0) {
            throw std::invalid_argument("memory controller requires byte-addressable DIMM width");
        }
        const auto size = module.geometry().capacity_bytes();
        const auto address_space = std::uint64_t{std::numeric_limits<Address>::max()} + 1;
        if (size > address_space - base) throw std::out_of_range("DIMM window exceeds physical address space");
        for (const auto& item : mappings_) {
            if (std::uint64_t{base} < std::uint64_t{item.base} + item.size
                && std::uint64_t{item.base} < std::uint64_t{base} + size) {
                throw std::invalid_argument("overlapping DIMM windows");
            }
            if (item.module == &module) throw std::invalid_argument("DIMM is already mapped");
        }
        mappings_.push_back({base, size, &module});
    }

    DecodedAddress decode(Address address) const {
        const auto& mapping = locate(address, 1);
        const auto local = static_cast<std::size_t>(address - mapping.base);
        const auto bytes = mapping.module->geometry().word_bits() / 8;
        return {local, local / bytes, local % bytes, mapping.module->decode(local / bytes)};
    }

    Bytes read(Address address, std::size_t size) {
        if (size == 0) return {};
        const auto& mapping = locate(address, size); // 整段先验界，避免地址被低位截断而别名。
        Bytes data(size);
        const auto local = static_cast<std::size_t>(address - mapping.base);
        const auto width = mapping.module->geometry().word_bits() / 8;
        for (std::size_t done = 0; done < size;) {
            const auto offset = (local + done) % width;
            const auto count = std::min(size - done, width - offset);
            const auto word = mapping.module->read((local + done) / width);
            ++statistics_.word_reads;
            for (std::size_t i = 0; i < count; ++i) {
                data[done + i] = static_cast<std::uint8_t>(word >> ((offset + i) * 8));
            }
            done += count;
        }
        ++statistics_.read_requests;
        statistics_.bytes_read += size;
        return data;
    }

    void write(Address address, const Bytes& data) {
        if (data.empty()) return;
        const auto& mapping = locate(address, data.size());
        const auto local = static_cast<std::size_t>(address - mapping.base);
        const auto width = mapping.module->geometry().word_bits() / 8;
        for (std::size_t done = 0; done < data.size();) {
            const auto offset = (local + done) % width;
            const auto count = std::min(data.size() - done, width - offset);
            const auto word_address = (local + done) / width;
            dimm::Word word = 0;
            if (offset != 0 || count != width) { // 只有部分字写入才需要读改写。
                word = mapping.module->read(word_address);
                ++statistics_.word_reads;
            }
            for (std::size_t i = 0; i < count; ++i) {
                const auto shift = (offset + i) * 8;
                word = (word & ~(dimm::Word{0xFF} << shift)) | (dimm::Word{data[done + i]} << shift);
            }
            mapping.module->write(word_address, word);
            ++statistics_.word_writes;
            done += count;
        }
        ++statistics_.write_requests;
        statistics_.bytes_written += data.size();
        if (write_observer_) write_observer_(address, data);
    }

    // 外层接写穿 Cache 的总线写监听；通知发生在物理数据提交后，包含 DMA 与宿主写入。
    void set_write_observer(std::function<void(Address, const Bytes&)> observer) {
        write_observer_ = std::move(observer);
    }

    std::function<Bytes(Address, std::size_t)> reader() {
        return [this](Address address, std::size_t size) { return read(address, size); };
    }
    std::function<void(Address, const Bytes&)> writer() {
        return [this](Address address, const Bytes& data) { write(address, data); };
    }
    const Statistics& statistics() const { return statistics_; }

private:
    struct Mapping { Address base; std::size_t size; dimm::DIMM* module; };

    const Mapping& locate(Address address, std::size_t size) const {
        for (const auto& item : mappings_) {
            if (address < item.base) continue;
            const auto local = std::uint64_t{address} - item.base;
            if (local < item.size && size <= item.size - local) return item;
        }
        throw std::out_of_range("physical transfer is outside a mapped DIMM window");
    }

    std::vector<Mapping> mappings_;
    Statistics statistics_;
    std::function<void(Address, const Bytes&)> write_observer_;
};

} // namespace colab::soc::controller
