#pragma once

#include "registers/registers.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace colab::soc {

// 本实验按图 5.6 将 noc 用作 CPU 的单条 16 位内部总线。
// 每个 out 信号对应一个三态门；总线没有锁存器。
enum class BusSource { None, PC, MDR, GPR, Z };

struct BusDrivers {
    bool pc_out = false;
    bool mdr_out = false;
    bool gpr_out = false;
    bool z_out = false;
};

class NoC {
public:
    std::optional<std::uint16_t> read(BusDrivers outputs, const core::Registers& registers,
                                    std::size_t gpr_index = 0) const {
        const unsigned drivers = unsigned(outputs.pc_out) + unsigned(outputs.mdr_out)
            + unsigned(outputs.gpr_out) + unsigned(outputs.z_out);
        if (drivers > 1) throw std::logic_error("multiple outputs drive the single bus");
        if (outputs.pc_out) return registers.pc.read();
        if (outputs.mdr_out) return registers.mdr.read();
        if (outputs.gpr_out) return registers.gprs.read(gpr_index);
        if (outputs.z_out) return registers.z.read();
        return std::nullopt;
    }

    std::optional<std::uint16_t> read(BusSource source, const core::Registers& registers,
                                    std::size_t gpr_index = 0) const {
        switch (source) {
        case BusSource::None: return read(BusDrivers{}, registers, gpr_index);
        case BusSource::PC: return read({true, false, false, false}, registers, gpr_index);
        case BusSource::MDR: return read({false, true, false, false}, registers, gpr_index);
        case BusSource::GPR: return read({false, false, true, false}, registers, gpr_index);
        case BusSource::Z: return read({false, false, false, true}, registers, gpr_index);
        }
        throw std::logic_error("unknown single bus source");
    }
};
}
