#pragma once

#include "../geometry.hpp"

#include <algorithm>

namespace colab::devices::hdd {

// 电驱动主轴，所有盘片刚性连接到同一转轴。速度按教学用线性加减速曲线变化。
class Spindle {
public:
    explicit Spindle(const Config& config)
        : nominal_rpm_(config.rpm), spin_up_ms_(config.spin_up_ms), coast_ms_(config.coast_down_ms) {}

    void drive(bool enabled) { driven_ = enabled; }
    bool driven() const { return driven_; }
    bool ready() const { return driven_ && speed_ == 1; }
    Time rpm() const { return speed_ * nominal_rpm_; }
    Time phase() const { return phase_; } // 一圈 = 1，从主轴索引位置起算。
    Time revolutions() const { return turns_; }
    Time time_to_speed() const { return driven_ ? (1 - speed_) * spin_up_ms_ : speed_ * coast_ms_; }

    void advance(Time elapsed) {
        check_time(elapsed);
        const auto ramp = std::min(elapsed, time_to_speed());
        const auto next_speed = driven_ ? std::min(1.0L, speed_ + ramp / spin_up_ms_) :
                                           std::max(0.0L, speed_ - ramp / coast_ms_);
        const auto turns = ((speed_ + next_speed) * ramp / 2 + next_speed * (elapsed - ramp)) *
                           nominal_rpm_ / 60000;
        speed_ = next_speed;
        turns_ += turns;
        phase_ = std::fmod(phase_ + turns, 1.0L);
    }

private:
    Time nominal_rpm_, spin_up_ms_, coast_ms_;
    bool driven_ = false;
    Time speed_ = 0, phase_ = 0, turns_ = 0;
};

} // namespace colab::devices::hdd
