#pragma once

namespace colab::dimm {

// 一个逻辑存储单元保存 1 bit；这里只表示电荷对应的逻辑值。
// Bank 为节省宿主内存，把同一行列交点上的多个 Cell 打包到一个 uint8_t。
class Cell {
public:
    explicit Cell(bool value = false) : value_(value) {}
    bool read() const { return value_; }
    void write(bool value) { value_ = value; }

private:
    bool value_ = false;
};

} // namespace colab::dimm
