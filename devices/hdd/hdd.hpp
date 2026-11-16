#pragma once

#include "actuator/actuator.hpp"
#include "controller/controller.hpp"
#include "dma/dma.hpp"
#include "head/read_write_head.hpp"

#include <algorithm>
#include <deque>
#include <limits>
#include <utility>

namespace colab::devices {

// 独立的电机/机械/磁记录事件模型。构造只有硬件，没有供电，也不会启动盘片。
// advance() 推进物理时间；内部按事件细分，tick 大于一个扇区时间也不会漏过 DMA。
class HDD {
public:
    explicit HDD(hdd::Config config = {})
        : config_(validated(std::move(config))), media_(config_.geometry),
          spindle_(config_), actuator_(config_) {}

    const hdd::Config& config() const { return config_; }
    const hdd::Geometry& geometry() const { return config_.geometry; }
    const hdd::Spindle& spindle() const { return spindle_; }
    const hdd::Actuator& actuator() const { return actuator_; }
    const hdd::ReadWriteHead& head() const { return head_; }
    const hdd::Controller& controller() const { return controller_; }
    const hdd::DMA& dma() const { return dma_; }
    const hdd::MagneticMedia& media() const { return media_; }
    const hdd::Statistics& statistics() const { return stats_; }
    hdd::Power power() const { return power_; }
    hdd::Time now_ms() const { return now_; }
    bool ready() const { return power_.operational() && spindle_.ready(); }
    bool busy() const { return controller_.busy(); }

    void set_power(hdd::Power power) {
        const bool lost = power_.operational() && !power.operational();
        power_ = power;
        spindle_.drive(power.operational());
        actuator_.power(power.operational());
        if (lost) {
            // 磁层保留，易失的数据寄存器、命令和读写电流失效。
            if (controller_.active_) finish(hdd::Status::PowerLost);
            while (!controller_.queue_.empty()) {
                auto request = std::move(controller_.queue_.front());
                controller_.queue_.pop_front();
                request.measurement.queue_ms = now_ - request.submitted_ms;
                emit(std::move(request), hdd::Status::PowerLost);
            }
        }
        if (!power.operational()) head_.disable();
        settle();
    }
    void power_on() { set_power({true, true}); }
    void power_off() { set_power({false, false}); }

    void set_bus_signals(hdd::BusSignals signals) {
        bus_ = signals;
        settle();
    }

    std::uint64_t submit_read(hdd::Address address) {
        require_power();
        geometry().check(address);
        return enqueue(address, hdd::Operation::Read,
                       hdd::Bytes(static_cast<std::size_t>(geometry().bytes_per_sector), 0));
    }
    std::uint64_t submit_write(hdd::Address address, hdd::Bytes host_data) {
        require_power();
        geometry().check(address);
        if (host_data.size() != geometry().bytes_per_sector)
            throw std::invalid_argument("HDD transfers whole sectors only");
        return enqueue(address, hdd::Operation::Write, std::move(host_data));
    }
    std::uint64_t submit_read_lba(std::uint64_t lba) { return submit_read(geometry().decode(lba)); }
    std::uint64_t submit_write_lba(std::uint64_t lba, hdd::Bytes host_data) {
        return submit_write(geometry().decode(lba), std::move(host_data));
    }
    std::optional<hdd::Completion> take_completion() {
        if (completions_.empty()) return std::nullopt;
        auto completion = std::move(completions_.front());
        completions_.pop_front();
        return completion;
    }

    void tick() { advance(config_.tick_ms); }
    void advance(hdd::Time elapsed) {
        hdd::check_time(elapsed);
        if (!std::isfinite(now_ + elapsed)) throw std::overflow_error("HDD clock overflow");
        settle();
        while (elapsed > 0) {
            const auto step = next_event(elapsed);
            spindle_.advance(step);
            actuator_.advance(step);
            if (controller_.active_) {
                auto& measurement = controller_.active_->measurement;
                switch (controller_.stage_) {
                case hdd::Stage::Decoding: measurement.controller_ms += step; break;
                case hdd::Stage::Prefetching: measurement.prefetch_ms += step; break;
                case hdd::Stage::Seeking: measurement.seek_ms += step; break;
                case hdd::Stage::Rotating: measurement.rotation_ms += step; break;
                case hdd::Stage::Transferring: measurement.transfer_ms += step; break;
                case hdd::Stage::Draining: measurement.drain_ms += step; break;
                case hdd::Stage::Idle: break;
                }
            }
            if (timed_stage()) controller_.remaining_ = std::max(0.0L, controller_.remaining_ - step);
            dma_.advance(step);
            now_ += step;
            elapsed -= step;
            settle();
        }
    }

    // 有界便捷运行，停在最后一个完成事件处，不额外空转到预算末尾。
    // 若 CPU 一直占用总线导致写预取等不到响应，返回 false，调用方仍可释放总线继续。
    bool run_until_idle(hdd::Time budget_ms) {
        hdd::check_time(budget_ms);
        settle();
        while (busy() && budget_ms > 0) {
            const auto step = next_event(budget_ms);
            advance(step);
            budget_ms -= step;
        }
        return !busy();
    }

private:
    hdd::Config config_;
    hdd::MagneticMedia media_;
    hdd::Spindle spindle_;
    hdd::Actuator actuator_;
    hdd::ReadWriteHead head_;
    hdd::Controller controller_;
    hdd::DMA dma_;
    hdd::Power power_{};
    hdd::BusSignals bus_{};
    hdd::Statistics stats_{};
    hdd::Time now_ = 0;
    std::uint64_t next_id_ = 1;
    std::deque<hdd::Completion> completions_;
    std::optional<hdd::Bytes> write_buffer_; // 写方向上的 DMA 数据寄存器。
    hdd::Bytes serial_word_; // 读写通道串并转换移位寄存器，与 DMA 数据寄存器分开。
    std::size_t offset_ = 0, word_size_ = 0;
    bool overhead_ = false;

    static hdd::Config validated(hdd::Config config) { config.validate(); return config; }
    void require_power() const {
        if (!power_.operational()) throw std::logic_error("HDD requires logic and motor power");
    }
    std::uint64_t enqueue(hdd::Address address, hdd::Operation operation, hdd::Bytes data) {
        if (next_id_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("HDD command ID exhausted");
        const auto id = next_id_++;
        controller_.queue_.push_back({id, address, operation, std::move(data), now_, {}});
        settle();
        return id;
    }
    bool timed_stage() const {
        return controller_.active_ && (controller_.stage_ == hdd::Stage::Decoding ||
            controller_.stage_ == hdd::Stage::Seeking || controller_.stage_ == hdd::Stage::Rotating ||
            controller_.stage_ == hdd::Stage::Transferring);
    }
    hdd::Time next_event(hdd::Time bound) const {
        if (timed_stage() && controller_.remaining_ > 0) bound = std::min(bound, controller_.remaining_);
        if (dma_.owns_bus()) bound = std::min(bound, dma_.remaining_ms());
        if (power_.operational() && !spindle_.ready()) {
            const auto ramp = spindle_.time_to_speed();
            if (ramp > 0) bound = std::min(bound, ramp);
        }
        return bound;
    }
    void stage(hdd::Stage next, hdd::Time duration = 0) {
        controller_.stage_ = next;
        controller_.remaining_ = duration;
    }
    std::size_t beat_size(std::size_t offset) const {
        return std::min(config_.dma_buffer_bits / 8,
                        static_cast<std::size_t>(geometry().bytes_per_sector) - offset);
    }
    void request_dma(std::size_t offset, std::size_t count, hdd::Bytes data = {}) {
        dma_.request(controller_.active_->operation, offset, count, std::move(data));
        ++controller_.active_->measurement.dma_requests;
    }
    void start_seek() {
        const auto address = controller_.active_->address;
        head_.select(address.head);
        stage(hdd::Stage::Seeking, actuator_.seek(address.cylinder));
    }
    void start_rotation() {
        const auto target = static_cast<hdd::Time>(controller_.active_->address.sector) /
                            geometry().sectors_per_track;
        auto fraction = target - spindle_.phase();
        // 累积浮点误差不能把刚好到达的扇区误判为需要再转一圈。
        constexpr hdd::Time angular_epsilon = 1e-15L;
        if (std::abs(fraction) < angular_epsilon || std::abs(fraction + 1) < angular_epsilon)
            fraction = 0;
        else if (fraction < 0) fraction += 1;
        stage(hdd::Stage::Rotating, fraction * config_.revolution_ms());
    }
    void start_transfer() {
        offset_ = 0;
        const auto overhead_time = config_.sector_ms() * (1 - config_.payload_fraction());
        stage(hdd::Stage::Transferring, overhead_time);
        overhead_ = true;
    }
    void start_word() {
        overhead_ = false;
        word_size_ = beat_size(offset_);
        if (controller_.active_->operation == hdd::Operation::Write) {
            if (!write_buffer_) { finish(hdd::Status::WriteUnderrun); return; }
            serial_word_ = std::move(*write_buffer_);
            write_buffer_.reset();
            const auto next = offset_ + word_size_;
            if (next < geometry().bytes_per_sector) request_dma(next, beat_size(next));
        }
        head_.enable(controller_.active_->operation);
        stage(hdd::Stage::Transferring, config_.sector_ms() * config_.payload_fraction() *
              word_size_ / geometry().bytes_per_sector);
    }
    void finish_word() {
        auto& request = *controller_.active_;
        if (request.operation == hdd::Operation::Read) {
            auto data = head_.read(media_, request.address, offset_, word_size_, actuator_, spindle_);
            request.measurement.media_bytes += word_size_;
            stats_.media_bytes_read += word_size_;
            // 盘片不会因 CPU 占用总线而停下来；下一字到达仍未清空数据寄存器就溢出。
            if (dma_.requested()) { finish(hdd::Status::ReadOverrun); return; }
            request_dma(offset_, word_size_, std::move(data));
        } else {
            head_.write(media_, request.address, offset_, serial_word_, actuator_, spindle_);
            request.measurement.media_bytes += word_size_;
            stats_.media_bytes_written += word_size_;
        }
        offset_ += word_size_;
        if (offset_ == geometry().bytes_per_sector) {
            head_.disable();
            if (request.operation == hdd::Operation::Read) stage(hdd::Stage::Draining);
            else finish(hdd::Status::Success);
        } else start_word();
    }
    void emit(hdd::Request request, hdd::Status status) {
        request.measurement.response_ms = now_ - request.submitted_ms;
        if (status == hdd::Status::Success) {
            if (request.operation == hdd::Operation::Read) ++stats_.successful_reads;
            else ++stats_.successful_writes;
            stats_.successful_access_ms += request.measurement.access_ms();
            stats_.successful_response_ms += request.measurement.response_ms;
        } else ++stats_.failed_commands;
        hdd::Bytes data;
        if (request.operation == hdd::Operation::Read) {
            data = std::move(request.host_memory);
            data.resize(static_cast<std::size_t>(request.measurement.dma_bytes));
        }
        completions_.push_back({request.id, request.address, request.operation, status,
                                std::move(data), request.measurement});
    }
    void finish(hdd::Status status) {
        auto request = std::move(*controller_.active_);
        controller_.active_.reset();
        stage(hdd::Stage::Idle);
        dma_.cancel();
        write_buffer_.reset();
        serial_word_.clear();
        head_.disable();
        emit(std::move(request), status);
    }

    // 同时到达时先完成总线周期，再锁存磁通道的下一字。零时长控制阶段在此收敛。
    void settle() {
        for (;;) {
            bool changed = false;
            if (dma_.owns_bus() && dma_.remaining_ms() == 0) {
                auto& request = *controller_.active_;
                request.measurement.dma_bytes += dma_.count_;
                auto incoming = dma_.complete(request.host_memory);
                if (request.operation == hdd::Operation::Write) write_buffer_ = std::move(incoming);
                changed = true;
            }
            if (controller_.active_) {
                const bool elapsed = controller_.remaining_ == 0;
                switch (controller_.stage_) {
                case hdd::Stage::Decoding:
                    if (elapsed) {
                        if (controller_.active_->operation == hdd::Operation::Write) {
                            stage(hdd::Stage::Prefetching);
                            request_dma(0, beat_size(0));
                        } else start_seek();
                        changed = true;
                    }
                    break;
                case hdd::Stage::Prefetching:
                    if (write_buffer_) { start_seek(); changed = true; }
                    break;
                case hdd::Stage::Seeking:
                    if (elapsed) { start_rotation(); changed = true; }
                    break;
                case hdd::Stage::Rotating:
                    if (elapsed) { start_transfer(); changed = true; }
                    break;
                case hdd::Stage::Transferring:
                    if (elapsed) {
                        if (overhead_) start_word();
                        else finish_word();
                        changed = true;
                    }
                    break;
                case hdd::Stage::Draining:
                    if (!dma_.requested()) { finish(hdd::Status::Success); changed = true; }
                    break;
                case hdd::Stage::Idle: break;
                }
            }
            if (!controller_.active_ && !controller_.queue_.empty() && ready()) {
                controller_.active_ = std::move(controller_.queue_.front());
                controller_.queue_.pop_front();
                controller_.active_->measurement.queue_ms = now_ - controller_.active_->submitted_ms;
                stage(hdd::Stage::Decoding, config_.controller_ms);
                changed = true;
            }
            if (controller_.active_ && dma_.grant(bus_, config_.dma_bus_cycle_ms)) {
                ++controller_.active_->measurement.dma_grants;
                if (bus_.cpu_request) ++controller_.active_->measurement.dma_cpu_contentions;
                changed = true;
            }
            if (!changed) break;
        }
    }
};

} // namespace colab::devices
