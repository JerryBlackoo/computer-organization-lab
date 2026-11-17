#pragma once

#include "dedicated/alu.hpp"
#include "dedicated/memory.hpp"
#include "dedicated/register_file.hpp"
#include "../control_unit/control_signals.hpp"
#include "../decoder/instruction.hpp"

#include <optional>

namespace colab::soc::core {

enum class RV32FaultCause {
    InstructionAddressMisaligned = 0, InstructionAccessFault = 1, IllegalInstruction = 2,
    Breakpoint = 3, LoadAddressMisaligned = 4, LoadAccessFault = 5, StoreAddressMisaligned = 6, StoreAccessFault = 7,
    EnvironmentCallFromU = 8, EnvironmentCallFromS = 9, EnvironmentCallFromM = 11,
    InstructionPageFault = 12, LoadPageFault = 13, StorePageFault = 15
};

enum class FaultHandling { HostException, MachineTrap };
struct RV32Trap { RV32FaultCause cause; std::uint32_t pc, value, target; };

// The CPU either delivers these precise faults to the guest or exposes them to
// a teaching/debug harness. Model implementation errors are never guest traps.
class RV32Fault : public std::runtime_error {
public:
    RV32Fault(RV32FaultCause cause, std::uint32_t pc, std::uint32_t value)
        : std::runtime_error("RV32 instruction fault; instruction not retired"),
          cause_(cause), pc_(pc), value_(value) {}
    RV32FaultCause cause() const { return cause_; }
    std::uint32_t pc() const { return pc_; }
    std::uint32_t value() const { return value_; }
private:
    RV32FaultCause cause_;
    std::uint32_t pc_, value_;
};

namespace datapath {
struct RV32State {
    dedicated::RegisterFile gprs;
    std::uint32_t pc = 0, mar = 0, mdr = 0, ir = 0;
    std::uint32_t a = 0, b = 0, alu_out = 0;
    dedicated::MulDiv muldiv;
    bool equal = false;
    std::optional<std::uint32_t> pending_instruction, pending_data;
};

struct RV32MemoryWrite { std::uint32_t address, value; unsigned bytes = 4; };
struct RV32RegisterWrite { std::uint8_t index; std::uint32_t value; };
struct RV32Step {
    RV32State next;
    std::optional<dedicated::ALUResult> alu;
    std::optional<RV32MemoryWrite> memory_write;
    std::optional<RV32RegisterWrite> register_write;
};

// 两个控制器仅提供控制线；这里不按机器操作类型分派执行。
// 32 位 ALU、寄存器堆和存储器直接复用 dedicated 的独立部件。
class RV32Datapath {
public:
    RV32Step evaluate(const RV32State& state, const DecodedInstruction& decoded,
                      const control_unit::rv32::ControlSignals& signals,
                      const dedicated::InstructionMemory& program,
                      const dedicated::DataMemory& memory,
                      bool external_data_port = false,
                      std::optional<std::uint32_t> external_data = std::nullopt,
                      bool external_instruction_port = false,
                      std::optional<std::uint32_t> external_instruction = std::nullopt) const {
        using namespace control_unit::rv32;
        validate(signals);
        RV32Step step{state, std::nullopt, std::nullopt, std::nullopt};
        if (signals.mar_from_pc) step.next.mar = state.pc;
        // 主存请求在首个 Read 拍采样；等待拍保持请求与返回快照，末拍才锁存 MDR。
        if (signals.instruction_read && !state.pending_instruction) {
            if (state.mar % 4 != 0)
                throw RV32Fault(RV32FaultCause::InstructionAddressMisaligned, state.pc, state.mar);
            try {
                if (external_instruction_port) {
                    if (!external_instruction) throw std::logic_error("external instruction response not ready");
                    step.next.pending_instruction = *external_instruction;
                } else step.next.pending_instruction = program.read(state.mar);
            }
            catch (const std::out_of_range&) {
                throw RV32Fault(RV32FaultCause::InstructionAccessFault, state.pc, state.mar);
            }
        }
        if (signals.instruction_to_mdr) {
            if (!state.pending_instruction) throw std::logic_error("instruction response not ready");
            step.next.mdr = *state.pending_instruction;
            step.next.pending_instruction.reset();
        }
        if (signals.ir_in) step.next.ir = state.mdr;
        if (signals.operands_in) {
            const auto ports = state.gprs.read(decoded.rs1, decoded.rs2);
            step.next.a = ports.bus_a;
            step.next.b = ports.bus_b;
        }
        if (signals.muldiv_start || signals.muldiv_step) {
            if (signals.muldiv_start) step.next.muldiv.start(signals.alu_op, state.a, state.b);
            else step.next.muldiv.step(alu_);
            if (step.next.muldiv.done()) {
                const auto result = step.next.muldiv.result();
                step.alu = dedicated::ALUResult{result, result == 0, false};
                step.next.alu_out = result;
            }
        }
        if (signals.alu_in || signals.address_in || signals.condition_in) {
            const auto b = signals.operand_b == OperandB::Immediate
                ? static_cast<std::uint32_t>(decoded.immediate) : state.b;
            step.alu = alu_.evaluate(signals.alu_op, state.a, b);
            if (step.alu->overflow) throw std::logic_error("RV32 arithmetic must wrap, not trap on overflow");
            if (signals.alu_in) step.next.alu_out = step.alu->result;
            if (signals.address_in) step.next.mar = step.alu->result;
            if (signals.condition_in) {
                const bool less_signed = (state.a ^ 0x80000000u) < (state.b ^ 0x80000000u);
                switch (signals.compare) {
                case Compare::Equal: step.next.equal = state.a == state.b; break;
                case Compare::NotEqual: step.next.equal = state.a != state.b; break;
                case Compare::LessSigned: step.next.equal = less_signed; break;
                case Compare::GreaterEqualSigned: step.next.equal = !less_signed; break;
                case Compare::LessUnsigned: step.next.equal = state.a < state.b; break;
                case Compare::GreaterEqualUnsigned: step.next.equal = state.a >= state.b; break;
                }
            }
        }
        if (signals.data_read && !state.pending_data) {
            if (external_data_port) {
                if (!external_data) throw std::logic_error("external load response not ready");
                step.next.pending_data = dedicated::extend_load(*external_data, signals.memory_bytes, signals.load_signed);
            } else {
                validate_data(state, memory, false, signals.memory_bytes);
                step.next.pending_data = dedicated::extend_load(memory.read(state.mar, signals.memory_bytes),
                    signals.memory_bytes, signals.load_signed);
            }
        }
        if (signals.data_to_mdr) {
            if (!state.pending_data) throw std::logic_error("load response not ready");
            step.next.mdr = *state.pending_data;
            step.next.pending_data.reset();
        }
        if (signals.memory_write) {
            if (!external_data_port) validate_data(state, memory, true, signals.memory_bytes);
            step.memory_write = RV32MemoryWrite{state.mar, state.b & dedicated::memory_mask(signals.memory_bytes),
                signals.memory_bytes};
        }
        if (signals.register_write) {
            std::uint32_t value = state.alu_out;
            switch (signals.write_back) {
            case WriteBack::ALU: break;
            case WriteBack::Memory: value = state.mdr; break;
            case WriteBack::UpperImmediate: value = static_cast<std::uint32_t>(decoded.immediate); break;
            case WriteBack::PcRelative: value = state.pc + static_cast<std::uint32_t>(decoded.immediate); break;
            case WriteBack::ReturnAddress: value = state.pc + 4u; break;
            }
            step.next.gprs.write(decoded.rd, value);
            if (decoded.rd != 0) step.register_write = RV32RegisterWrite{decoded.rd, value};
        }
        if (signals.pc_write) {
            std::uint32_t next_pc = state.pc + 4u;
            if (signals.pc_source == PCSource::Branch || signals.pc_source == PCSource::JumpImmediate)
                next_pc = state.pc + static_cast<std::uint32_t>(decoded.immediate);
            else if (signals.pc_source == PCSource::JumpRegister)
                next_pc = (state.a + static_cast<std::uint32_t>(decoded.immediate)) & ~1u;
            if (next_pc % 4 != 0)
                throw RV32Fault(RV32FaultCause::InstructionAddressMisaligned, state.pc, next_pc);
            step.next.pc = next_pc;
        }
        return step;
    }
private:
    static void validate(const control_unit::rv32::ControlSignals& s) {
        (void)dedicated::memory_mask(s.memory_bytes);
        if ((s.instruction_read && s.data_read) ||
            (s.memory_write && (s.data_read || s.instruction_read)) ||
            (s.instruction_to_mdr && s.data_to_mdr) ||
            (s.instruction_to_mdr && !s.instruction_read) ||
            (s.data_to_mdr && !s.data_read) ||
            (s.mar_from_pc && s.address_in) ||
            (s.muldiv_start && s.muldiv_step) ||
            ((s.muldiv_start || s.muldiv_step) && (s.alu_in || s.address_in || s.condition_in || s.retire)) ||
            (s.retire != s.pc_write) ||
            ((s.register_write || s.memory_write) && !s.retire) ||
            (s.register_write && s.memory_write))
            throw std::logic_error("incompatible RV32 control signals");
    }
    static void validate_data(const RV32State& state, const dedicated::DataMemory& memory, bool store, unsigned bytes) {
        if (state.mar % bytes != 0)
            throw RV32Fault(store ? RV32FaultCause::StoreAddressMisaligned : RV32FaultCause::LoadAddressMisaligned,
                            state.pc, state.mar);
        try { memory.validate(state.mar, bytes); }
        catch (const std::out_of_range&) {
            throw RV32Fault(store ? RV32FaultCause::StoreAccessFault : RV32FaultCause::LoadAccessFault,
                            state.pc, state.mar);
        }
    }
    dedicated::ALU alu_;
};
} // namespace datapath
} // namespace colab::soc::core
