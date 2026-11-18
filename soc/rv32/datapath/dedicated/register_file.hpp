#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace colab::soc::core::datapath::dedicated {

struct ReadPorts { std::uint32_t bus_a, bus_b; };

// 图 5.8 的 32 x 32 位寄存器堆：两个组合读端口、一个时钟写端口。
class RegisterFile {
public:
    static constexpr std::size_t count = 32;
    std::uint32_t read(std::size_t index) const { return words_.at(index); }
    ReadPorts read(std::size_t ra, std::size_t rb) const { return {read(ra), read(rb)}; }
    void write(std::size_t rw, std::uint32_t bus_w, bool enable = true) {
        auto& word = words_.at(rw);
        if (enable && rw != 0) word = bus_w; // RV32 x0 恒为 0。
    }
private:
    std::array<std::uint32_t, count> words_{};
};
}
