#pragma once

#include <cstdint>

namespace colab::soc::core::registers {

// 寄存器只保存状态；是否写入、何时写入由控制信号决定。
class WordRegister {
public:
    std::uint16_t read() const { return value_; }
    void write(std::uint16_t value) { value_ = value; }

private:
    std::uint16_t value_ = 0;
};

} // namespace colab::soc::core::registers
