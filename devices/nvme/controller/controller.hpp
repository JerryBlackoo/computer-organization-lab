#pragma once

#include "../dma/dma.hpp"
#include "../nand/nand.hpp"
#include "../../common/irq_sink.hpp"
#include <array>

namespace colab::devices::nvme {
// NVMe-shaped subset: admin Create CQ/SQ + NVM Read/Write, one namespace/IO queue.
// Actual 64-byte SQEs, 16-byte CQEs and PCIe register/doorbell offsets are retained.
// Unsupported features fail explicitly; this is not a compliant production controller.
class Controller final : public common::MmioDevice {
public:
    static constexpr std::uint32_t Cap = 0, Version = 8, IntMaskSet = 12, IntMaskClear = 16,
        Config = 20, Status = 28, AdminAttributes = 36, AdminSq = 40, AdminCq = 48, Doorbell = 0x1000;
    enum class State { Disabled, Idle, Fetching, MediaWait, DataTransfer, Completing, Fatal };
    Controller(NAND& nand, common::DmaBus& bus, common::Master master, common::IrqSink& irq, unsigned source)
        : nand_(nand), dma_(bus, master, 64), irq_(irq), source_(source) {}
    State state() const { return state_; }
    const DMA& dma() const { return dma_; }
    std::uint64_t completed_commands() const { return completed_; }
    std::uint32_t mmio_read32(std::uint32_t offset) override {
        switch (offset) {
        case Cap: return 15u | (1u << 16); // MQES=15, contiguous queues required.
        case Cap + 4: return 1u << 5; // CSS.NVM=1, 4KiB MPS, DSTRD=0.
        case Version: return 0x00010400u; // register layout reference; limited subset documented.
        case IntMaskSet: case IntMaskClear: return interrupt_mask_;
        case Config: return config_;
        case Status: return (state_ == State::Disabled ? 0u : 1u) | (state_ == State::Fatal ? 2u : 0u);
        case AdminAttributes: return attributes_;
        case AdminSq: return static_cast<std::uint32_t>(admin_sq_);
        case AdminSq + 4: return static_cast<std::uint32_t>(admin_sq_ >> 32);
        case AdminCq: return static_cast<std::uint32_t>(admin_cq_);
        case AdminCq + 4: return static_cast<std::uint32_t>(admin_cq_ >> 32);
        default: throw std::out_of_range("unknown/write-only NVMe register");
        }
    }
    void mmio_write32(std::uint32_t offset, std::uint32_t value) override {
        if (offset == IntMaskSet) { interrupt_mask_ |= value; update_irq(); return; }
        if (offset == IntMaskClear) { interrupt_mask_ &= ~value; update_irq(); return; }
        if (offset >= Doorbell && offset < Doorbell + 16 && offset % 4 == 0) {
            if (state_ == State::Disabled || state_ == State::Fatal) throw std::logic_error("NVMe not ready");
            auto& queue = queues_[(offset - Doorbell) / 8];
            if ((offset - Doorbell) % 8 == 0) {
                if (!queue.sq_valid || value >= queue.sq_depth) throw std::out_of_range("invalid SQ tail");
                const auto added = (value + queue.sq_depth - queue.sq_tail) % queue.sq_depth;
                const auto used = (queue.sq_tail + queue.sq_depth - queue.sq_head) % queue.sq_depth;
                if (added > queue.sq_depth - 1 - used) throw std::out_of_range("SQ tail overruns unconsumed commands");
                queue.sq_tail = value;
            } else {
                if (!queue.cq_valid || value >= queue.cq_depth) throw std::out_of_range("invalid CQ head");
                const auto consumed = (value + queue.cq_depth - queue.cq_head) % queue.cq_depth;
                if (consumed > queue.used) throw std::out_of_range("CQ head beyond posted completions");
                queue.used -= consumed; queue.cq_head = value; update_irq();
            }
            return;
        }
        if (offset == Config) {
            if (enabling_) throw std::logic_error("NVMe enable transition still pending");
            if (!(value & 1)) {
                if (dma_.busy() || (state_ != State::Idle && state_ != State::Fatal && state_ != State::Disabled))
                    throw std::logic_error("quiesce NVMe before disable");
                config_ = value; state_ = State::Disabled; queues_ = {}; update_irq(); return;
            }
            if (state_ != State::Disabled) throw std::logic_error("NVMe already enabled");
            // Supported CC: EN=1, CSS=0, MPS=0, AMS=0, SHN=0, IOSQES=6, IOCQES=4.
            if (value != 0x00460001u) throw std::invalid_argument("unsupported NVMe CC format");
            const auto sq_depth = (attributes_ & 0xFFFu) + 1, cq_depth = ((attributes_ >> 16) & 0xFFFu) + 1;
            if (!valid_queue(admin_sq_, sq_depth) || !valid_queue(admin_cq_, cq_depth))
                throw std::invalid_argument("invalid admin queue geometry");
            auto& queue = queues_[0];
            queue.sq_base = admin_sq_; queue.cq_base = admin_cq_;
            queue.sq_depth = sq_depth; queue.cq_depth = cq_depth;
            queue.sq_valid = queue.cq_valid = queue.interrupt = true;
            config_ = value; enabling_ = true; return;
        }
        if (state_ != State::Disabled || enabling_) throw std::logic_error("NVMe queue registers require disabled state");
        switch (offset) {
        case AdminAttributes:
            if (value & 0xF000F000u) throw std::invalid_argument("reserved AQA bits");
            attributes_ = value; break;
        case AdminSq: admin_sq_ = (admin_sq_ & 0xFFFFFFFF00000000ULL) | value; break;
        case AdminSq + 4: admin_sq_ = (admin_sq_ & 0xFFFFFFFFULL) | (std::uint64_t(value) << 32); break;
        case AdminCq: admin_cq_ = (admin_cq_ & 0xFFFFFFFF00000000ULL) | value; break;
        case AdminCq + 4: admin_cq_ = (admin_cq_ & 0xFFFFFFFFULL) | (std::uint64_t(value) << 32); break;
        default: throw std::out_of_range("read-only/unknown NVMe register");
        }
    }
    void tick() override {
        dma_.tick();
        if (enabling_) { enabling_ = false; state_ = State::Idle; return; }
        if (state_ == State::Disabled || state_ == State::Fatal) return;
        switch (state_) {
        case State::Idle:
            for (unsigned qid = 0; qid < queues_.size(); ++qid) {
                auto& q = queues_[qid];
                // Leave one CQ slot free so head doorbells can unambiguously release entries.
                if (!q.sq_valid || q.sq_head == q.sq_tail || q.used + 1 >= q.cq_depth) continue;
                active_ = qid;
                dma_.start({q.sq_base + q.sq_head * 64, 64, common::DmaDirection::MemoryToDevice});
                state_ = State::Fetching; break;
            }
            break;
        case State::Fetching:
            if (dma_.failed()) { fatal(); break; }
            if (!dma_.completed()) break;
            command_ = dma_.data();
            queues_[active_].sq_head = (queues_[active_].sq_head + 1) % queues_[active_].sq_depth;
            cid_ = common::load32(command_) >> 16; completion_status_ = 0;
            if (active_ == 0) execute_admin(); else execute_io();
            break;
        case State::MediaWait:
            if (--media_wait_) break;
            if (read_) data_ = nand_.read(lba_, blocks_);
            else { nand_.write(lba_, data_); post_completion(); break; }
            start_segment();
            break;
        case State::DataTransfer:
            if (dma_.failed()) { completion_status_ = 4; post_completion(); break; } // Data Transfer Error.
            if (!dma_.completed()) break;
            if (!read_) std::copy(dma_.data().begin(), dma_.data().end(), data_.begin() + data_offset_);
            data_offset_ += segment_bytes_;
            if (data_offset_ < data_.size()) start_segment();
            else if (read_) post_completion();
            else { media_wait_ = 3; state_ = State::MediaWait; }
            break;
        case State::Completing:
            if (dma_.failed()) { fatal(); break; }
            if (!dma_.completed()) break;
            {
                auto& q = queues_[active_];
                ++q.used;
                if (++q.cq_tail == q.cq_depth) { q.cq_tail = 0; q.phase = !q.phase; }
                ++completed_; update_irq(); state_ = State::Idle;
            }
            break;
        case State::Disabled: case State::Fatal: break;
        }
    }
private:
    struct Queue {
        std::uint64_t sq_base = 0, cq_base = 0;
        std::uint32_t sq_depth = 0, cq_depth = 0, sq_head = 0, sq_tail = 0, cq_head = 0, cq_tail = 0, used = 0;
        bool sq_valid = false, cq_valid = false, interrupt = false, phase = true;
    };
    static bool valid_queue(std::uint64_t base, std::uint32_t depth) {
        return base && base % 4096 == 0 && depth >= 2 && depth <= 16 && base <= UINT64_MAX - depth * 64;
    }
    void update_irq() {
        bool pending = false;
        for (const auto& q : queues_) pending |= q.used && q.interrupt;
        irq_.set_irq(source_, pending && !(interrupt_mask_ & 1));
    }
    void fatal() { state_ = State::Fatal; }
    void execute_admin() {
        const auto opcode = command_[0];
        const auto cdw10 = common::load32(command_, 40), cdw11 = common::load32(command_, 44);
        const auto qid = cdw10 & 0xFFFFu, depth = (cdw10 >> 16) + 1;
        const auto base = common::load64(command_, 24);
        if ((common::load32(command_) & 0xFF00u) || common::load32(command_, 4)) completion_status_ = 2;
        else if (opcode != 5 && opcode != 1) completion_status_ = 1; // Invalid Command Opcode.
        else if (qid != 1 || !valid_queue(base, depth) || !(cdw11 & 1)) completion_status_ = 2;
        else {
            auto& q = queues_[1];
            if (opcode == 5) {
                if (q.cq_valid || (cdw11 >> 16) != 0 || (cdw11 & 0xFFFCu)) completion_status_ = 2;
                else { q.cq_base = base; q.cq_depth = depth; q.cq_valid = true; q.interrupt = cdw11 & 2; }
            } else {
                if (q.sq_valid || !q.cq_valid || (cdw11 >> 16) != 1 || (cdw11 & 0xFFFEu)) completion_status_ = 2;
                else { q.sq_base = base; q.sq_depth = depth; q.sq_valid = true; }
            }
        }
        post_completion();
    }
    void execute_io() {
        const auto opcode = command_[0];
        lba_ = common::load64(command_, 40);
        const auto cdw12 = common::load32(command_, 48);
        blocks_ = (cdw12 & 0xFFFFu) + 1;
        prp1_ = common::load64(command_, 24); prp2_ = common::load64(command_, 32);
        const auto bytes = std::size_t(blocks_) * NAND::block_bytes;
        if (opcode != 1 && opcode != 2) completion_status_ = 1;
        else if (common::load32(command_, 4) != 1) completion_status_ = 0xB; // Invalid Namespace.
        else if ((common::load32(command_) & 0xFF00u) || common::load64(command_, 16) ||
                 (cdw12 & 0xFFFF0000u) || !prp1_ || prp1_ % 4 || bytes > 4096 ||
                 (bytes > 4096 - prp1_ % 4096 && (!prp2_ || prp2_ % 4096))) completion_status_ = 2;
        else if (lba_ >= nand_.blocks() || blocks_ > nand_.blocks() - lba_) completion_status_ = 0x80;
        if (completion_status_) { post_completion(); return; }
        read_ = opcode == 2; data_.assign(bytes, 0); data_offset_ = 0;
        if (read_) { media_wait_ = 3; state_ = State::MediaWait; }
        else start_segment();
    }
    void start_segment() {
        const auto first_bytes = std::min<std::size_t>(data_.size(), 4096 - prp1_ % 4096);
        segment_bytes_ = data_offset_ == 0 ? first_bytes : data_.size() - data_offset_;
        const auto address = data_offset_ == 0 ? prp1_ : prp2_;
        dma_.start({address, static_cast<std::uint32_t>(segment_bytes_), read_
            ? common::DmaDirection::DeviceToMemory : common::DmaDirection::MemoryToDevice}, read_
            ? common::Bytes(data_.begin() + data_offset_, data_.begin() + data_offset_ + segment_bytes_) : common::Bytes{});
        state_ = State::DataTransfer;
    }
    void post_completion() {
        auto& q = queues_[active_];
        common::Bytes completion(16, 0);
        common::store32(completion, 8, q.sq_head | (active_ << 16));
        common::store32(completion, 12, cid_ | ((std::uint32_t(completion_status_) << 1 | q.phase) << 16));
        dma_.start({q.cq_base + q.cq_tail * 16, 16, common::DmaDirection::DeviceToMemory}, std::move(completion));
        state_ = State::Completing;
    }
    NAND& nand_;
    DMA dma_;
    common::IrqSink& irq_;
    unsigned source_, active_ = 0, media_wait_ = 0;
    State state_ = State::Disabled;
    bool enabling_ = false, read_ = false;
    std::array<Queue, 2> queues_{};
    std::uint32_t config_ = 0, attributes_ = 0, interrupt_mask_ = 0, cid_ = 0, blocks_ = 0;
    std::uint16_t completion_status_ = 0;
    std::uint64_t admin_sq_ = 0, admin_cq_ = 0, prp1_ = 0, prp2_ = 0, lba_ = 0, completed_ = 0;
    std::size_t data_offset_ = 0, segment_bytes_ = 0;
    common::Bytes command_, data_;
};
}
