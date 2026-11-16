#pragma once

#include "cache.hpp"
#include "../../interconnect/interconnect.hpp"

namespace colab::soc::cache {

// CPU MMU 之后的物理数据端口。单个在途请求；只有系统推进总线和本控制器。
// 复用 Cache 的 Tag/替换/数据阵列；下级回调只消费已经由总线确认的填充或写穿。
class CpuPort final : public devices::common::DmaBus {
public:
    using Bytes = devices::common::Bytes;
    using Tag = devices::common::Tag;
    using Operation = devices::common::BusOperation;
    using Result = devices::common::BusResult;
    using Status = interconnect::pcie::Status;
    struct Statistics {
        std::uint64_t bypasses = 0;
        std::uint64_t snoop_writes = 0;
        std::uint64_t fill_forwarded_bytes = 0;
    };

    static Config default_config() {
        return {16, 2, 32, WritePolicy::WriteThrough, AllocationPolicy::NoWriteAllocate};
    }

    explicit CpuPort(devices::common::DmaBus& bus, Config config = default_config(), bool enabled = true)
        : bus_(bus), enabled_(enabled), cache_(
            [this](Address address, std::size_t size) {
                if (!request_ || request_->kind != Kind::Fill || address != request_->block || fill_.size() != size)
                    throw std::logic_error("cache fill has no acknowledged bus response");
                return fill_;
            },
            [this](Address address, const Bytes& data) {
                if (!request_ || request_->kind != Kind::Store || !write_acknowledged_ ||
                    address != request_->address || data != request_->data)
                    throw std::logic_error("cache write has no bus acknowledgement");
            }, validate(config)) {}
    CpuPort(const CpuPort&) = delete;
    CpuPort& operator=(const CpuPort&) = delete;
    CpuPort(CpuPort&&) = delete;
    CpuPort& operator=(CpuPort&&) = delete;

    Tag submit(devices::common::Master master, Operation operation, std::uint64_t address,
               std::size_t count, Bytes data = {}) override {
        if (master != 0) throw std::invalid_argument("CPU cache port accepts master 0 only");
        if (request_ || result_) throw std::length_error("CPU cache port busy");
        if (next_tag_ == std::numeric_limits<Tag>::max()) throw std::overflow_error("CPU cache tags exhausted");
        if ((operation != Operation::Read && operation != Operation::Write) || !count || count > 1024 ||
            (operation == Operation::Write ? data.size() != count : !data.empty()))
            throw std::invalid_argument("invalid CPU data transaction");

        const bool scalar = address <= UINT32_MAX && (count == 1 || count == 2 || count == 4) && address % count == 0;
        const auto physical = static_cast<Address>(address);
        const auto block = cache_.address_decoder().decode(physical).block_address;
        // 只缓存完整落在同一 RAM 窗口内的行；MMIO、未映射地址和窗口边缘走原总线。
        const bool cached = enabled_ && scalar && address - block + count <= cache_.config().line_bytes &&
            bus_.is_memory(block, cache_.config().line_bytes);
        if (cached && operation == Operation::Read && cache_.contains(physical, count)) {
            result_ = Result{next_tag_, 0, Status::Success, cache_.read(physical, count), count, {}, false};
            return next_tag_++;
        }

        Request next;
        next.tag = next_tag_;
        next.address = physical;
        next.block = block;
        next.count = count;
        next.data = std::move(data);
        next.kind = !cached ? Kind::Bypass : operation == Operation::Read ? Kind::Fill : Kind::Store;
        if (next.kind == Kind::Fill) {
            next.forwarded.resize(cache_.config().line_bytes);
            next.forwarded_valid.resize(cache_.config().line_bytes);
        }
        // 若总线队列已满，submit 原样失败；不留下半接收请求，CPU 可在下一拍重试。
        next.downstream = bus_.submit(0, operation, next.kind == Kind::Fill ? block : address,
            next.kind == Kind::Fill ? cache_.config().line_bytes : count, next.data);
        request_ = std::move(next);
        if (!cached) ++statistics_.bypasses;
        return next_tag_++;
    }

    bool ready(Tag tag) const override { return result_ && result_->tag == tag; }
    Result take(Tag tag) override {
        if (!ready(tag)) throw std::out_of_range("CPU data result is not ready");
        auto result = std::move(*result_);
        result_.reset();
        return result;
    }

    // 总线先 tick，本控制器再消费响应。不会递归运行总线或 CPU。
    void tick() {
        if (!request_ || !bus_.ready(request_->downstream)) return;
        auto response = bus_.take(request_->downstream);
        response.tag = request_->tag;
        if (request_->kind != Kind::Bypass) {
            const auto expected = request_->kind == Kind::Fill ? cache_.config().line_bytes : request_->count;
            if (response.status == Status::Success && (response.transferred_bytes != expected ||
                (request_->kind == Kind::Fill && response.data.size() != expected))) {
                response.status = Status::BackendError;
                response.error = "incomplete cache backend transaction";
            }
            if (response.status == Status::Success) {
                if (request_->kind == Kind::Fill) {
                    fill_ = std::move(response.data);
                    // DMA 可在填充的多个数据拍之间写入已读过的字节；保留最新监听值。
                    for (std::size_t i = 0; i < fill_.size(); ++i)
                        if (request_->forwarded_valid[i]) fill_[i] = request_->forwarded[i];
                    response.data = cache_.read(request_->address, request_->count);
                    fill_.clear();
                } else {
                    write_acknowledged_ = true;
                    cache_.write(request_->address, request_->data);
                    write_acknowledged_ = false;
                }
                response.transferred_bytes = request_->count;
            } else if (request_->kind == Kind::Fill) {
                // 不把部分缓存行作为一条成功的 CPU load，也不污染有效行。
                response.data.clear();
                response.transferred_bytes = 0;
            }
        }
        result_ = std::move(response);
        request_.reset();
    }

    // 物理写入已提交后的总线监听。只更新已有行；DMA 不经 CPU Cache 分配数据。
    void snoop_write(Address address, const Bytes& data) {
        if (!enabled_) return;
        cache_.snoop_write(address, data);
        ++statistics_.snoop_writes;
        if (!request_ || request_->kind != Kind::Fill) return;
        const auto begin = std::max(std::uint64_t{address}, std::uint64_t{request_->block});
        const auto end = std::min(std::uint64_t{address} + data.size(),
                                  std::uint64_t{request_->block} + cache_.config().line_bytes);
        for (auto current = begin; current < end; ++current) {
            const auto offset = static_cast<std::size_t>(current - request_->block);
            request_->forwarded[offset] = data[static_cast<std::size_t>(current - address)];
            request_->forwarded_valid[offset] = true;
            ++statistics_.fill_forwarded_bytes;
        }
    }

    const Cache& cache() const { return cache_; }
    const Statistics& statistics() const { return statistics_; }
    bool enabled() const { return enabled_; }

private:
    enum class Kind { Bypass, Fill, Store };
    struct Request {
        Tag tag = 0, downstream = 0;
        Address address = 0, block = 0;
        std::size_t count = 0;
        Kind kind = Kind::Bypass;
        Bytes data, forwarded;
        std::vector<bool> forwarded_valid;
    };
    static Config validate(Config config) {
        if (config.write_policy != WritePolicy::WriteThrough ||
            config.allocation_policy != AllocationPolicy::NoWriteAllocate || config.write_buffer_entries != 0 ||
            config.line_bytes < 4 || config.line_bytes > 1024)
            throw std::invalid_argument("CPU port requires write-through/no-write-allocate, no buffer, 4..1024B lines");
        return config;
    }
    devices::common::DmaBus& bus_;
    bool enabled_;
    Cache cache_;
    std::optional<Request> request_;
    std::optional<Result> result_;
    Bytes fill_;
    bool write_acknowledged_ = false;
    Tag next_tag_ = 1;
    Statistics statistics_;
};

} // namespace colab::soc::cache
