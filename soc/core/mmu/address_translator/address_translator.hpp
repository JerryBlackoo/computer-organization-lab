#pragma once

#include "../types.hpp"

namespace colab::soc::core::mmu {

struct VirtualAddressParts {
    Address virtual_page;
    Address page_offset;
};

class AddressTranslator {
public:
    static constexpr VirtualAddressParts split(Address address) {
        return {address >> page_offset_bits, address & (page_size - 1)};
    }

    // 页内偏移保持不变，替换的只有页号。
    static Address compose(Address physical_frame, Address page_offset) {
        if (physical_frame > max_page_number || page_offset >= page_size) {
            throw std::out_of_range("invalid physical frame or page offset");
        }
        return (physical_frame << page_offset_bits) | page_offset;
    }
};

} // namespace colab::soc::core::mmu
