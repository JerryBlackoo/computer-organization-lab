#include "../soc/core/mmu/mmu.hpp"
#include "../soc/cache/cache.hpp"

#include <cassert>

int main() {
    using namespace colab::soc::cache;
    using namespace colab::soc::core::mmu;
    Bytes memory(4096, 0);
    memory[3] = 42;
    std::size_t lower_reads = 0;
    colab::soc::Cache cache(
        [&](Address address, std::size_t size) {
            ++lower_reads;
            return Bytes(memory.begin() + address, memory.begin() + address + size);
        },
        [&](Address address, const Bytes& data) {
            std::copy(data.begin(), data.end(), memory.begin() + address);
        });
    colab::soc::core::MMU mmu;
    mmu.enable_paging(0, [](Address pte_address) {
        if (pte_address == 8) return PageTableEntry{0, true, {true, false, false, true}};
        return PageTableEntry{};
    });
    assert(cache.read_byte(mmu.translate(0x1003)) == 42);
    const auto reads = lower_reads;
    const auto fills = cache.statistics().fills;
    bool denied = false;
    try { cache.write_byte(mmu.translate(0x1003, AccessType::Write), 99); }
    catch (const TranslationFault& fault) {
        denied = true;
        assert(fault.reason() == FaultReason::WriteDenied);
    }
    assert(denied);
    bool not_present = false;
    try { cache.read_byte(mmu.translate(0x2000)); }
    catch (const TranslationFault& fault) {
        not_present = true;
        assert(fault.reason() == FaultReason::NotPresent);
    }
    assert(not_present);
    assert(lower_reads == reads && cache.statistics().fills == fills);
    assert(cache.read_byte(3) == 42);
}
