#pragma once

#include "bus.hpp"
#include "../control_unit/control_signal_generator/control_signal_generator.hpp"

namespace colab::soc::core::datapath {

struct SystemBusRequest {
    std::uint16_t address; // MAR -> 系统地址总线
    std::uint16_t data;    // MDR -> 系统数据总线（Write）
    bool read;
    bool write;
};

struct SingleBusStep {
    Registers next_registers;
    std::optional<std::uint16_t> bus;
    std::optional<ALUResult> alu;
    std::optional<SystemBusRequest> memory_request;
};

// 图 5.6：Y -> ALU.A，总线 -> ALU.B，ALU.F -> Z/FR，Zout -> 总线。
// evaluate 不改变旧状态；调用者在时钟边沿一次性提交 next_registers。
class SingleBusDatapath {
public:
    SingleBusStep evaluate(const Registers& registers,
                          const control_unit::ControlSignals& signals,
                          std::size_t read_index = 0, std::size_t write_index = 0,
                          std::optional<std::uint16_t> memory_data = std::nullopt) const {
        if (signals.read && signals.write) throw std::logic_error("simultaneous Read and Write");
        if (signals.memory_to_mdr && (!signals.mdr_in || !signals.read))
            throw std::logic_error("memory response requires MDRin and Read");
        if (signals.pc_in && signals.increment_pc)
            throw std::logic_error("two inputs selected for PC");
        SingleBusStep step{registers, noc_.read(signals.bus_source, registers, read_index),
                           std::nullopt, std::nullopt};
        if (signals.read || signals.write)
            step.memory_request = SystemBusRequest{registers.mar.read(), registers.mdr.read(),
                                                   signals.read, signals.write};
        const auto bus_value = [&]() {
            if (!step.bus) throw std::logic_error("bus input enabled without an output source");
            return *step.bus;
        };
        if (signals.mar_in) step.next_registers.mar.write(bus_value());
        if (signals.pc_in) step.next_registers.pc.write(bus_value());
        if (signals.ir_in) step.next_registers.ir.write(bus_value());
        if (signals.y_in) step.next_registers.y.write(bus_value());
        if (signals.gpr_in) step.next_registers.gprs.write(write_index, bus_value());
        // Read 持续期间，MDRin 有效但仅在主存数据就绪的边沿锁存。
        if (signals.mdr_in && !signals.read) step.next_registers.mdr.write(bus_value());
        if (signals.memory_to_mdr) {
            if (!memory_data) throw std::logic_error("main memory response is not ready");
            step.next_registers.mdr.write(*memory_data);
        }
        if (signals.z_in || signals.fr_in) {
            const auto a = signals.alu_a_from_bus ? bus_value() : registers.y.read();
            const auto b = signals.alu_op == ALUOperation::Increment ? 0 : bus_value();
            step.alu = alu_.evaluate(signals.alu_op, a, b);
            if (signals.z_in) step.next_registers.z.write(step.alu->value);
            if (signals.fr_in) step.next_registers.fr.write(step.alu->flags);
        }
        if (signals.increment_pc)
            step.next_registers.pc.write(static_cast<std::uint16_t>(registers.pc.read() + 2u));
        return step;
    }
private:
    colab::soc::NoC noc_;
    ALU alu_;
};
}
