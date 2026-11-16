#pragma once

#include "../types.hpp"
#include "../dma/dma.hpp"
#include "../../common/irq_sink.hpp"

#include <deque>
#include <string>

namespace colab::devices::hdd {

struct Request {
    std::uint64_t id;
    Address address;
    Operation operation;
    Bytes host_memory; // 主机内存的逻辑端口，不是磁盘缓存或提前读到的磁性数据。
    Time submitted_ms;
    Measurement measurement{};
};

class Controller {
public:
    Stage stage() const { return stage_; }
    bool busy() const { return active_.has_value() || !queue_.empty(); }
    std::size_t queued() const { return queue_.size(); }
    std::optional<std::uint64_t> active_id() const {
        return active_ ? std::optional<std::uint64_t>(active_->id) : std::nullopt;
    }

private:
    friend class colab::devices::HDD;
    Stage stage_ = Stage::Idle;
    std::deque<Request> queue_;
    std::optional<Request> active_;
    Time remaining_ = 0;
};

enum class DeviceState { Idle, CommandAccepted, Busy, DataReady, Completed, Error };

// Teaching MMIO protocol around the existing mechanical HDD. The HDD's standalone
// completion vector is an adapter-local sector buffer here, never system DRAM.
// CPU PIO and BusDMA are the only paths from that buffer to mapped host memory.
template<class Disk>
class IOController final : public common::MmioDevice {
public:
    enum Register : std::uint32_t {
        Command = 0, Status = 4, Data = 8, Lba = 12, Length = 16,
        DmaAddress = 20, Control = 24, IrqStatus = 28, DmaAddressHigh = 32,
        Progress = 36, ErrorCode = 40
    };
    static constexpr std::uint32_t Read = 1, Write = 2;
    static constexpr std::uint32_t Busy = 1, Ready = 2, Done = 4, Failed = 8;
    static constexpr std::uint32_t EnableIrq = 1, EnableDma = 2;

    IOController(Disk& disk, common::DmaBus& bus, common::Master master,
                 common::IrqSink& irq, unsigned source)
        : disk_(disk), dma_(bus, master), irq_(irq), source_(source) {}
    DeviceState state() const { return state_; }
    const BusDMA& dma() const { return dma_; }
    const std::string& error() const { return error_; }
    std::uint32_t mmio_read32(std::uint32_t offset) override {
        switch (offset) {
        case Command: return command_;
        case Status:
            if (state_ == DeviceState::DataReady) return Ready;
            if (state_ == DeviceState::Completed) return Done;
            if (state_ == DeviceState::Error) return Failed;
            return active() ? Busy : 0;
        case Data: return read_data();
        case Lba: return lba_;
        case Length: return length_;
        case DmaAddress: return static_cast<std::uint32_t>(dma_address_);
        case DmaAddressHigh: return static_cast<std::uint32_t>(dma_address_ >> 32);
        case Control: return control_;
        case IrqStatus: return irq_pending_;
        case Progress: return static_cast<std::uint32_t>((control_ & EnableDma) ? (dma_started_ ? dma_.transferred() : 0) : offset_);
        case ErrorCode: return state_ == DeviceState::Error ? 1 : 0;
        default: throw std::out_of_range("unknown HDD MMIO register");
        }
    }
    void mmio_write32(std::uint32_t offset, std::uint32_t value) override {
        if (offset == IrqStatus) { if (value & 1) { irq_pending_ = false; update_irq(); } return; }
        if (offset == Data) { write_data(value); return; }
        if (active()) throw std::logic_error("cannot reprogram active HDD command");
        switch (offset) {
        case Lba: lba_ = value; break;
        case Length: length_ = value; break;
        case DmaAddress: dma_address_ = (dma_address_ & 0xFFFFFFFF00000000ULL) | value; break;
        case DmaAddressHigh: dma_address_ = (dma_address_ & 0xFFFFFFFFULL) | (std::uint64_t(value) << 32); break;
        case Control:
            if (value & ~(EnableIrq | EnableDma)) throw std::invalid_argument("unknown HDD control bits");
            control_ = value; update_irq(); break;
        case Command: accept(value); break;
        default: throw std::out_of_range("read-only/unknown HDD MMIO register");
        }
    }
    void tick() override {
        disk_.tick();
        dma_.tick();
        if (!active() || state_ == DeviceState::DataReady) return;
        try {
            if (state_ == DeviceState::CommandAccepted) {
                state_ = DeviceState::Busy;
                if (command_ == Read) media_id_ = disk_.submit_read_lba(lba_);
                else if (control_ & EnableDma) {
                    dma_.start({dma_address_, length_, common::DmaDirection::MemoryToDevice});
                    transferring_ = dma_started_ = true;
                } else pio_next_ = true;
                return;
            }
            if (media_id_) {
                auto result = disk_.take_completion();
                if (!result) return;
                if (result->id != *media_id_ || result->status != hdd::Status::Success)
                    throw std::runtime_error("mechanical HDD operation failed");
                media_id_.reset();
                if (command_ == Write) { complete(); return; }
                buffer_ = std::move(result->data);
                if (control_ & EnableDma) {
                    dma_.start({dma_address_, length_, common::DmaDirection::DeviceToMemory}, buffer_);
                    transferring_ = dma_started_ = true;
                } else pio_next_ = true;
                return;
            }
            if (transferring_) {
                if (dma_.failed()) throw std::runtime_error(dma_.error());
                if (!dma_.completed()) return;
                transferring_ = false;
                if (command_ == Read) complete();
                else { buffer_ = dma_.data(); media_id_ = disk_.submit_write_lba(lba_, buffer_); }
                return;
            }
            if (pio_next_ && (!(control_ & EnableIrq) || !irq_pending_)) {
                pio_next_ = false; state_ = DeviceState::DataReady; signal();
            }
        } catch (const std::exception& error) { fail(error.what()); }
    }
private:
    bool active() const {
        return state_ == DeviceState::CommandAccepted || state_ == DeviceState::Busy || state_ == DeviceState::DataReady;
    }
    void accept(std::uint32_t command) {
        irq_pending_ = false; update_irq(); error_.clear(); offset_ = 0;
        transferring_ = pio_next_ = dma_started_ = false; media_id_.reset();
        command_ = command;
        if ((command != Read && command != Write) || !length_ || length_ != disk_.geometry().bytes_per_sector ||
            lba_ >= disk_.geometry().sectors() || !disk_.ready() || disk_.busy()) {
            fail("invalid HDD command, geometry, length, or power state"); return;
        }
        buffer_.assign(length_, 0);
        state_ = DeviceState::CommandAccepted;
    }
    std::uint32_t read_data() {
        if (state_ != DeviceState::DataReady || command_ != Read || (control_ & EnableDma))
            throw std::logic_error("HDD PIO data not ready for read");
        std::uint32_t value = 0;
        const auto count = std::min<std::size_t>(4, length_ - offset_);
        for (std::size_t i = 0; i < count; ++i) value |= std::uint32_t(buffer_[offset_ + i]) << (8 * i);
        offset_ += count;
        if (offset_ == length_) complete();
        else { state_ = DeviceState::Busy; pio_next_ = true; }
        return value;
    }
    void write_data(std::uint32_t value) {
        if (state_ != DeviceState::DataReady || command_ != Write || (control_ & EnableDma))
            throw std::logic_error("HDD PIO data not ready for write");
        const auto count = std::min<std::size_t>(4, length_ - offset_);
        for (std::size_t i = 0; i < count; ++i) buffer_[offset_ + i] = static_cast<std::uint8_t>(value >> (8 * i));
        offset_ += count; state_ = DeviceState::Busy;
        if (offset_ == length_) {
            try { media_id_ = disk_.submit_write_lba(lba_, buffer_); }
            catch (const std::exception& error) { fail(error.what()); }
        }
        else pio_next_ = true;
    }
    void update_irq() { irq_.set_irq(source_, irq_pending_ && (control_ & EnableIrq)); }
    void signal() { irq_pending_ = true; update_irq(); }
    void complete() { state_ = DeviceState::Completed; signal(); }
    void fail(std::string error) { error_ = std::move(error); state_ = DeviceState::Error; signal(); }
    Disk& disk_;
    BusDMA dma_;
    common::IrqSink& irq_;
    unsigned source_;
    DeviceState state_ = DeviceState::Idle;
    std::uint32_t command_ = 0, lba_ = 0, length_ = 0, control_ = 0;
    std::uint64_t dma_address_ = 0;
    std::size_t offset_ = 0;
    bool irq_pending_ = false, transferring_ = false, pio_next_ = false, dma_started_ = false;
    std::optional<std::uint64_t> media_id_;
    Bytes buffer_;
    std::string error_;
};

} // namespace colab::devices::hdd
