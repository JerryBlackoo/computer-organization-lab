#pragma once

#include "../soc/core/mmu/mmu.hpp"

#include <functional>

namespace colab::simulation {

// 外层接线：先让 MMU 检查全部虚页，再把成功翻译的物理请求交给外部接口。
// MMU 和 Cache 都不持有本对象；回调可接 Cache、测试数组或其他物理存储。
class MemoryAccess {
public:
    using Address = soc::core::mmu::Address;
    using Privilege = soc::core::mmu::Privilege;
    using Bytes = std::vector<std::uint8_t>;
    using ReadPhysical = std::function<Bytes(Address, std::size_t)>;
    using WritePhysical = std::function<void(Address, const Bytes&)>;

    MemoryAccess(soc::core::MMU& mmu, ReadPhysical read, WritePhysical write)
        : mmu_(mmu), read_(std::move(read)), write_(std::move(write)) {
        if (!read_ || !write_) throw std::invalid_argument("memory access requires physical read/write callbacks");
    }

    Bytes read(Address address, std::size_t size, Privilege privilege = Privilege::User) {
        return read_as(address, size, soc::core::mmu::AccessType::Read, privilege);
    }
    Bytes fetch(Address address, std::size_t size, Privilege privilege = Privilege::User) {
        return read_as(address, size, soc::core::mmu::AccessType::Execute, privilege);
    }
    void write(Address address, const Bytes& data, Privilege privilege = Privilege::User) {
        const auto spans = mmu_.translate_range(address, data.size(), soc::core::mmu::AccessType::Write, privilege);
        std::size_t done = 0;
        for (const auto& span : spans) {
            write_(span.physical_address, Bytes(data.begin() + done, data.begin() + done + span.size));
            done += span.size;
        }
    }

private:
    Bytes read_as(Address address, std::size_t size, soc::core::mmu::AccessType access, Privilege privilege) {
        const auto spans = mmu_.translate_range(address, size, access, privilege);
        Bytes result(size);
        std::size_t done = 0;
        for (const auto& span : spans) {
            const auto data = read_(span.physical_address, span.size);
            if (data.size() != span.size) throw std::runtime_error("incomplete physical data read");
            std::copy(data.begin(), data.end(), result.begin() + done);
            done += data.size();
        }
        return result;
    }

    soc::core::MMU& mmu_;
    ReadPhysical read_;
    WritePhysical write_;
};

} // namespace colab::simulation
