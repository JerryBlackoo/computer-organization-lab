#pragma once

#include "../geometry.hpp"

#include <algorithm>

namespace colab::devices::hdd {

// 一个音圈执行器带动全部磁头臂；只能有一个共同的径向位置。
class Actuator {
public:
    explicit Actuator(const Config& config)
        : cylinders_(config.geometry.cylinders), max_seek_ms_(2 * config.average_seek_ms) {}

    void power(bool powered) {
        powered_ = powered;
        if (!powered) remaining_ = 0; // 失电中止驱动，保持当前机械位置供观察。
    }
    Time seek(std::uint64_t cylinder) {
        if (!powered_) throw std::logic_error("actuator has no electrical power");
        if (cylinder >= cylinders_) throw std::out_of_range("invalid actuator cylinder");
        target_ = cylinder;
        remaining_ = cylinders_ == 1 ? 0 :
            std::abs(static_cast<Time>(cylinder) - position_) * max_seek_ms_ / (cylinders_ - 1);
        if (!remaining_) position_ = target_;
        return remaining_;
    }
    void advance(Time elapsed) {
        check_time(elapsed);
        if (remaining_) {
            const auto part = std::min(elapsed, remaining_);
            position_ += (target_ - position_) * part / remaining_;
            remaining_ -= part;
            if (!remaining_) position_ = target_;
        }
    }
    Time position() const { return position_; }
    bool at(std::uint64_t cylinder) const { return powered_ && !remaining_ && position_ == cylinder; }

private:
    std::uint64_t cylinders_, target_ = 0;
    Time max_seek_ms_, position_ = 0, remaining_ = 0;
    bool powered_ = false;
};

} // namespace colab::devices::hdd
