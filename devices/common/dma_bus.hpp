#pragma once

#include "mmio_device.hpp"
#include "../../interconnect/pcie/pcie.hpp"

namespace colab::devices::common {
using Master = interconnect::pcie::Master;
using Tag = interconnect::pcie::Tag;
using BusResult = interconnect::pcie::Result;
using BusOperation = interconnect::pcie::Operation;

class DmaBus {
public:
    virtual ~DmaBus() = default;
    // Submission only queues a request. The system, never a requester, clocks the bus.
    virtual Tag submit(Master master, BusOperation operation, std::uint64_t address,
                       std::size_t count, Bytes data = {}) = 0;
    virtual bool ready(Tag tag) const = 0;
    virtual BusResult take(Tag tag) = 0;
    // Optional RAM-window query used by CPU cache adapters; MMIO stays uncached.
    virtual bool is_memory(std::uint32_t, std::size_t) const { return false; }
};

enum class DmaDirection { DeviceToMemory, MemoryToDevice };
struct DmaDescriptor {
    std::uint64_t memory_address;
    std::uint32_t length;
    DmaDirection direction;
};
enum class DmaState { Idle, Transferring, Completed, Error };

// One outstanding beat per engine. Address/count advance on acknowledged beats only.
class DmaEngine {
public:
    DmaEngine(DmaBus& bus, Master master, std::size_t beat_bytes = 4)
        : bus_(bus), master_(master), beat_bytes_(beat_bytes) {
        if (beat_bytes == 0 || beat_bytes > 1024) throw std::invalid_argument("invalid DMA beat size");
    }
    void start(DmaDescriptor descriptor, Bytes payload = {}) {
        if (busy()) throw std::logic_error("DMA engine busy");
        if ((descriptor.direction != DmaDirection::DeviceToMemory && descriptor.direction != DmaDirection::MemoryToDevice) ||
            !descriptor.length || descriptor.length > 1024 * 1024 ||
            descriptor.memory_address > UINT64_MAX - (descriptor.length - 1))
            throw std::invalid_argument("invalid DMA descriptor");
        if ((descriptor.direction == DmaDirection::DeviceToMemory && payload.size() != descriptor.length) ||
            (descriptor.direction == DmaDirection::MemoryToDevice && !payload.empty()))
            throw std::invalid_argument("DMA payload/direction mismatch");
        descriptor_ = descriptor; transferred_ = 0; pending_.reset(); error_.clear();
        data_ = descriptor.direction == DmaDirection::DeviceToMemory ? std::move(payload) : Bytes(descriptor.length);
        state_ = DmaState::Transferring;
    }
    void tick() {
        if (!busy()) return;
        if (pending_) {
            if (!bus_.ready(*pending_)) return;
            auto result = bus_.take(*pending_); pending_.reset();
            const auto committed = std::min(result.transferred_bytes, beat_count_);
            if (descriptor_.direction == DmaDirection::MemoryToDevice) {
                const auto count = std::min(committed, result.data.size());
                std::copy_n(result.data.begin(), count, data_.begin() + transferred_);
            }
            transferred_ += committed;
            if (result.status != interconnect::pcie::Status::Success || result.transferred_bytes != beat_count_) {
                fail(result.error.empty() ? "short DMA transaction" : result.error); return;
            }
            if (descriptor_.direction == DmaDirection::MemoryToDevice) {
                if (result.data.size() != beat_count_) { fail("short DMA read"); return; }
            }
            if (transferred_ == descriptor_.length) { state_ = DmaState::Completed; return; }
        }
        beat_count_ = std::min<std::size_t>(beat_bytes_, remaining());
        const bool write = descriptor_.direction == DmaDirection::DeviceToMemory;
        try {
            pending_ = bus_.submit(master_, write ? BusOperation::Write : BusOperation::Read,
                current_address(), beat_count_, write
                    ? Bytes(data_.begin() + transferred_, data_.begin() + transferred_ + beat_count_) : Bytes{});
        } catch (const std::length_error&) {
            // Backpressure: keep the descriptor/data register and retry next cycle.
        } catch (const std::exception& error) { fail(error.what()); }
    }
    bool busy() const { return state_ == DmaState::Transferring; }
    bool completed() const { return state_ == DmaState::Completed; }
    bool failed() const { return state_ == DmaState::Error; }
    DmaState state() const { return state_; }
    std::size_t transferred() const { return transferred_; }
    std::size_t remaining() const { return descriptor_.length - transferred_; }
    std::uint64_t current_address() const { return descriptor_.memory_address + transferred_; }
    const Bytes& data() const { return data_; }
    const std::string& error() const { return error_; }
private:
    void fail(std::string error) { state_ = DmaState::Error; error_ = std::move(error); }
    DmaBus& bus_;
    Master master_;
    std::size_t beat_bytes_, transferred_ = 0, beat_count_ = 0;
    DmaDescriptor descriptor_{0, 0, DmaDirection::MemoryToDevice};
    std::optional<Tag> pending_;
    Bytes data_;
    DmaState state_ = DmaState::Idle;
    std::string error_;
};
} // namespace colab::devices::common
