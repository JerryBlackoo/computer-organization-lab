#pragma once

#include <cstdint>

namespace colab::simulation {

// Component must provide void tick(). It must outlive the simulator.
template <typename Component>
class Simulator {
public:
    explicit Simulator(Component& component) : component_(component) {}

    // Advance one simulated cycle. No real-time delay is implied.
    void step() {
        component_.tick();
        ++cycles_;
    }

    // Run a bounded number of cycles; callers control when to stop.
    void run(std::uint64_t cycles) {
        for (std::uint64_t i = 0; i < cycles; ++i) {
            step();
        }
    }

    std::uint64_t cycles() const { return cycles_; }

private:
    Component& component_;
    std::uint64_t cycles_ = 0;
};

} // namespace colab::simulation
