#pragma once

#include "alu/alu.hpp"
#include "control_unit/control_unit.hpp"
#include "datapath/single_bus.hpp"
#include "decoder/decoder.hpp"
#include "mmu/mmu.hpp"
#include "registers/registers.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

namespace colab::soc {

struct CycleTrace {
    core::control_unit::Phase phase;
    core::control_unit::ControlSignals signals;
    std::optional<std::uint16_t> bus;
    std::optional<core::ALUResult> alu;
    core::control_unit::TimingState timing;
    bool instruction_complete;
    core::control_unit::Phase next_phase;
    bool stage_complete;
};

class Core {
public:
    // PC/MAR 是 16 位字节地址；每条教学指令占 2 字节。
    explicit Core(const std::vector<isa::Instruction>& program, core::mmu::TlbConfig tlb = {}) : mmu_(tlb) {
        if (program.size() > 0x8000u) {
            throw std::length_error("program exceeds 16-bit address space");
        }
        program_.reserve(program.size());
        for (const auto instruction : program) program_.push_back(isa::encode(instruction));
    }

    // 一个系统时钟周期：组合地产生控制/数据，在末尾的有效脉冲统一提交。
    void tick() {
        if (halted_) return;

        const auto signals = control_.signals(decoded_);
        auto next_decoded = decoded_;
        auto next_pending_read = pending_read_;
        const auto read_index = signals.register_select == core::control_unit::RegisterSelect::Rs1
            ? decoded_.rs1 : decoded_.rs2;
        // 图 5.6 的固定五拍主存端口：C2 发 Read，C6 数据进入 MDR。
        // 这里只模拟固定延迟的取指端口，不模拟 DRAM 芯片本身。
        if (control_.phase() == core::control_unit::Phase::FetchRead) {
            const auto address = mmu_.translate(registers_.mar.read(), core::mmu::AccessType::Execute);
            if (address % 2 != 0 || address / 2 >= program_.size()) {
                throw std::out_of_range("instruction address outside program");
            }
            next_pending_read = program_.at(address / 2);
        }
        const auto step = datapath_.evaluate(registers_, signals, read_index, decoded_.rd, pending_read_);
        if (signals.memory_to_mdr) next_pending_read.reset();

        if (signals.decode) {
            // 求本拍 IR 锁存之后的组合译码输出（IR -> CU），准备下一拍控制。
            // decoded_ 是此组合输出的软件缓存，不增加译码拍或 MDR -> CU 旁路。
            next_decoded = decoder_.decode(step.next_registers.ir.read());
        }
        const auto next_phase = control_.next_phase(next_decoded);
        const bool instruction_complete = control_.instruction_complete();
        CycleTrace trace{control_.phase(), signals, step.bus, step.alu,
                         control_.timing(), instruction_complete, next_phase,
                         instruction_complete ||
                         core::control_unit::instruction_stage(next_phase) != control_.stage()};

        // 模拟时钟边沿：统一提交。异常发生时，本拍状态不会部分更新。
        registers_ = step.next_registers;
        decoded_ = next_decoded;
        pending_read_ = next_pending_read;
        control_.on_clock_pulse(next_phase);
        halted_ = signals.halt;
        last_cycle_ = trace;
    }

    bool is_halted() const { return halted_; }
    core::control_unit::Phase phase() const { return control_.phase(); }
    core::control_unit::InstructionStage stage() const { return control_.stage(); }
    std::uint64_t clock_pulses() const { return control_.clock_pulses(); }
    std::uint64_t completed_instructions() const { return control_.completed_instructions(); }
    const core::Registers& registers() const { return registers_; }
    const std::optional<CycleTrace>& last_cycle() const { return last_cycle_; }
    core::MMU& mmu() { return mmu_; }
    const core::MMU& mmu() const { return mmu_; }

    // 宿主初始化接口。uint16_t 参数表示写入的是原始 16 位位模式。
    void set_register(std::size_t index, std::uint16_t value) {
        registers_.gprs.write(index, value);
    }

private:
    std::vector<std::uint16_t> program_;
    core::Registers registers_;
    core::datapath::SingleBusDatapath datapath_;
    core::Decoder decoder_;
    core::MMU mmu_;
    core::ControlUnit control_;
    isa::Instruction decoded_{};
    std::optional<std::uint16_t> pending_read_;
    std::optional<CycleTrace> last_cycle_;
    bool halted_ = false;
};

} // namespace colab::soc
