#pragma once

#include "../actuator/actuator.hpp"
#include "../medium/magnetic_media.hpp"
#include "../spindle/spindle.hpp"

namespace colab::devices::hdd {

// 磁头选择是电子切换；换磁头不移动同一柱面上的其他磁头臂。
class ReadWriteHead {
public:
    bool read_amplifier_enabled() const { return read_enabled_; }
    bool write_current_enabled() const { return write_enabled_; }
    std::uint64_t selected() const { return selected_; }

private:
    friend class colab::devices::HDD;
    bool read_enabled_ = false, write_enabled_ = false;
    std::uint64_t selected_ = 0;

    void disable() { read_enabled_ = write_enabled_ = false; }
    void select(std::uint64_t head) { selected_ = head; }
    void enable(Operation operation) {
        read_enabled_ = operation == Operation::Read;
        write_enabled_ = operation == Operation::Write;
    }
    void check(Address address, const Actuator& actuator, const Spindle& spindle) const {
        if (!spindle.ready() || !actuator.at(address.cylinder) || selected_ != address.head)
            throw std::logic_error("head is not positioned on rotating media");
    }
    Bytes read(const MagneticMedia& media, Address address, std::size_t offset, std::size_t count,
               const Actuator& actuator, const Spindle& spindle) const {
        check(address, actuator, spindle);
        if (!read_enabled_) throw std::logic_error("read amplifier is disabled");
        return media.read(address, offset, count);
    }
    void write(MagneticMedia& media, Address address, std::size_t offset, const Bytes& bytes,
               const Actuator& actuator, const Spindle& spindle) const {
        check(address, actuator, spindle);
        if (!write_enabled_) throw std::logic_error("write current is disabled");
        media.write(address, offset, bytes);
    }
};

} // namespace colab::devices::hdd
