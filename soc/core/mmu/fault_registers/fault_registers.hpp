#pragma once

#include "../types.hpp"

namespace colab::soc::core::mmu {

// 故障地址/原因等硬件状态；不保存 OS 的页框分配、磁盘换页策略。
class FaultRegisters {
public:
    void latch(const TranslationTrace& trace, FaultReason reason, Address page_table_base) {
        fault_ = FaultInfo{trace.virtual_address, trace.access, trace.privilege, reason,
                           trace.stage, page_table_base, trace.tlb_hit, trace.page_table_entry_address};
    }
    const std::optional<FaultInfo>& fault() const { return fault_; }
    void clear() { fault_.reset(); }

private:
    std::optional<FaultInfo> fault_; // 保留最近故障，成功访问不会隐式清除。
};

} // namespace colab::soc::core::mmu
