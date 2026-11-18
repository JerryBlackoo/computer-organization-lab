#pragma once

#include "../shared/control_unit/control_unit.hpp"
#include "datapath/rv32.hpp"
#include "../shared/decoder/decoder.hpp"
#include "interrupt_system/interrupt_system.hpp"
#include "csr/csr_access.hpp"
#include "../shared/mmu/mmu.hpp"
#include "../shared/mmu/sv32.hpp"
#include "pipeline/pipeline.hpp"
#include "../../devices/common/dma_bus.hpp"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace colab::soc {

struct RV32CycleTrace {
    core::control_unit::ControlMode mode;
    core::control_unit::rv32::Phase phase;
    core::control_unit::TimingState timing;
    core::control_unit::rv32::ControlSignals signals;
    std::optional<core::control_unit::microprogrammed::MicroAddress> micro_address;
    std::optional<core::datapath::dedicated::ALUResult> alu;
    bool instruction_complete;
    bool bus_wait = false;
    bool interrupt_entry = false;
    // Phase/signals/micro_address describe only MultiCycle execution.
    core::pipeline::Execution execution = core::pipeline::Execution::MultiCycle;
    std::optional<core::RV32Trap> trap{};
    bool wfi_wait = false;
    std::optional<core::datapath::dedicated::MulDiv> muldiv{};
};

struct RV32RetirementTrace {
    std::uint64_t instruction_number, clock_pulse, cycles;
    std::uint32_t pc, next_pc, instruction;
    core::control_unit::ControlMode mode;
    std::optional<core::datapath::RV32RegisterWrite> register_write;
    std::optional<core::datapath::RV32MemoryWrite> memory_write;
};

class RV32Core {
public:
    using Mode = core::control_unit::ControlMode;
    using Execution = core::pipeline::Execution;
    // Preserve standalone teaching-MMU experiments; the SystemC machine selects
    // Architectural explicitly, so host mmu() configuration cannot change Bare/M semantics.
    enum class Translation { Teaching, Architectural };
    explicit RV32Core(std::vector<std::uint32_t> program, Mode mode = Mode::Hardwired,
                      std::size_t data_words = 256,
                      core::control_unit::microprogrammed::ControlStore store = {},
                      Execution execution = Execution::MultiCycle, core::pipeline::Options options = {},
                      core::FaultHandling faults = core::FaultHandling::HostException, Translation translation = Translation::Teaching,
                      core::mmu::TlbConfig tlb = {})
        : sv32_(tlb, translation == Translation::Architectural ? core::Sv32::WalkTiming::OnePtePerTick
                                                               : core::Sv32::WalkTiming::Eager),
          mmu_(tlb), program_(std::move(program)), memory_(data_words), control_(mode, std::move(store)),
          data_words_(data_words), execution_(execution), pipeline_(options), faults_(faults), translation_(translation) {
        if (translation != Translation::Teaching && translation != Translation::Architectural)
            throw std::invalid_argument("unknown address translation model");
        if (translation == Translation::Architectural && execution != Execution::MultiCycle)
            throw std::invalid_argument("architectural shared-memory translation requires a multicycle core");
        if (execution != Execution::MultiCycle && execution != Execution::Pipelined)
            throw std::invalid_argument("unknown RV32 execution mode");
        if (execution == Execution::Pipelined && mode != Mode::Hardwired)
            throw std::invalid_argument("RV32 pipeline uses hardwired control; microprograms require MultiCycle execution");
    }

    // 一次 tick 是一个时钟周期：推进各流水级，或执行一拍多周期控制。
    void tick() {
        sv32_.drain(); // Reap abandoned page-table reads after traps/redirects.
        if (interrupts_.waiting()) {
            if (!interrupts_.wake_pending()) {
                if (execution_ == Execution::Pipelined) record_pipeline_cycle(pipeline_.idle_clock());
                else {
                    record_hold({}); last_cycle_->bus_wait = false; last_cycle_->wfi_wait = true;
                    control_.wait_clock();
                }
                ++wfi_wait_cycles_;
                return;
            }
            interrupts_.resume();
        }
        if (execution_ == Execution::Pipelined) {
            if (interrupts_.sv32_enabled(core::mmu::AccessType::Execute) || interrupts_.sv32_enabled(core::mmu::AccessType::Read))
                throw std::logic_error("Sv32 requires the multicycle memory-backed core, not the legacy ROM pipeline");
            tick_pipeline(); return;
        }
        try { tick_multicycle(); }
        catch (const core::RV32Fault& fault) {
            if (faults_ == core::FaultHandling::HostException) throw;
            enter_exception(fault);
        }
        catch (const core::mmu::TranslationFault& fault) {
            if (faults_ == core::FaultHandling::HostException) throw;
            enter_exception(core::rv32_translation_fault(fault, state_.pc));
        }
    }
private:
    void tick_multicycle() {
        // A trap is accepted only between retired instructions, never during a bus cycle.
        if (control_.at_instruction_boundary() && interrupts_.requested()) {
            state_.pc = interrupts_.enter(state_.pc);
            state_.pending_instruction.reset(); state_.pending_data.reset();
            record_hold({}, true);
            control_.wait_clock();
            return;
        }
        const auto signals = control_.signals(decoded_, {state_.equal, state_.muldiv.busy(), state_.muldiv.done()});
        std::optional<std::uint32_t> external_instruction;
        if (instruction_bus_ && signals.instruction_read && !state_.pending_instruction) {
            if (state_.mar % 4)
                throw core::RV32Fault(core::RV32FaultCause::InstructionAddressMisaligned, state_.pc, state_.mar);
            if (!pending_instruction_bus_) {
                const auto physical = translate_address(state_.mar, 4, core::mmu::AccessType::Execute);
                if (!physical) { record_hold(signals); control_.wait_clock(); ++bus_waits_; return; }
                try {
                    pending_instruction_bus_ = instruction_bus_->submit(0,
                        devices::common::BusOperation::Read, *physical, 4);
                } catch (const std::length_error&) {
                    record_hold(signals); control_.wait_clock(); ++bus_waits_; return;
                } catch (const std::exception&) {
                    throw core::RV32Fault(core::RV32FaultCause::InstructionAccessFault, state_.pc, state_.mar);
                }
            }
            if (!instruction_bus_->ready(*pending_instruction_bus_)) {
                record_hold(signals); control_.wait_clock(); ++bus_waits_; return;
            }
            const auto result = instruction_bus_->take(*pending_instruction_bus_);
            pending_instruction_bus_.reset();
            if (result.status != interconnect::fabric::Status::Success || result.transferred_bytes != 4 || result.data.size() != 4)
                throw core::RV32Fault(core::RV32FaultCause::InstructionAccessFault, state_.pc, state_.mar);
            external_instruction = devices::common::load32(result.data);
        }
        if (!instruction_bus_ && signals.instruction_read && !state_.pending_instruction) {
            if (state_.mar % 4)
                throw core::RV32Fault(core::RV32FaultCause::InstructionAddressMisaligned, state_.pc, state_.mar);
            // Legacy vector-ROM execution is deliberately Bare-only. The SystemC
            // platform always connects an instruction port for Sv32 execution.
            if (interrupts_.sv32_enabled(core::mmu::AccessType::Execute))
                throw core::RV32Fault(core::RV32FaultCause::InstructionAccessFault, state_.pc, state_.mar);
            interrupts_.check_memory(state_.mar, 4, core::mmu::AccessType::Execute, state_.pc);
        }
        std::optional<std::uint32_t> external_data;
        if (bus_ && ((signals.data_read && !state_.pending_data) || signals.memory_write)) {
            if (!pending_bus_) {
                const auto cause = signals.memory_write ? core::RV32FaultCause::StoreAddressMisaligned
                                                        : core::RV32FaultCause::LoadAddressMisaligned;
                if (state_.mar % signals.memory_bytes) throw core::RV32Fault(cause, state_.pc, state_.mar);
                const auto physical = translate_address(state_.mar, signals.memory_bytes, signals.memory_write
                    ? core::mmu::AccessType::Write : core::mmu::AccessType::Read);
                if (!physical) { record_hold(signals); control_.wait_clock(); ++bus_waits_; return; }
                try {
                    pending_bus_ = bus_->submit(0, signals.memory_write ? devices::common::BusOperation::Write
                        : devices::common::BusOperation::Read, *physical, signals.memory_bytes,
                        signals.memory_write ? devices::common::scalar_bytes(state_.b, signals.memory_bytes) : devices::common::Bytes{});
                } catch (const std::length_error&) { record_hold(signals); control_.wait_clock(); ++bus_waits_; return; }
                catch (const std::exception&) { throw_access(signals.memory_write); }
            }
            if (!bus_->ready(*pending_bus_)) { record_hold(signals); control_.wait_clock(); ++bus_waits_; return; }
            auto result = bus_->take(*pending_bus_); pending_bus_.reset();
            if (result.status != interconnect::fabric::Status::Success || result.transferred_bytes != signals.memory_bytes ||
                (!signals.memory_write && result.data.size() != signals.memory_bytes))
                throw_access(signals.memory_write);
            if (!signals.memory_write) external_data = devices::common::load_scalar(result.data);
        }
        if (!bus_ && ((signals.data_read && !state_.pending_data) || signals.memory_write)) {
            if (interrupts_.sv32_enabled(signals.memory_write ? core::mmu::AccessType::Write : core::mmu::AccessType::Read))
                throw_access(signals.memory_write); // Sv32 execution requires a connected memory port.
            if (state_.mar % signals.memory_bytes)
                throw core::RV32Fault(signals.memory_write ? core::RV32FaultCause::StoreAddressMisaligned :
                    core::RV32FaultCause::LoadAddressMisaligned, state_.pc, state_.mar);
            interrupts_.check_memory(state_.mar, signals.memory_bytes, signals.memory_write
                ? core::mmu::AccessType::Write : core::mmu::AccessType::Read, state_.pc);
        }
        auto step = datapath_.evaluate(state_, decoded_, signals, program_, memory_, bus_ != nullptr, external_data,
                                      instruction_bus_ != nullptr, external_instruction);
        auto next_interrupts = interrupts_;
        if (signals.system) {
            if (decoded_.operation == core::Operation::Ecall)
                throw core::RV32Fault(interrupts_.ecall_cause(), state_.pc, 0);
            if (decoded_.operation == core::Operation::Ebreak)
                throw core::RV32Fault(core::RV32FaultCause::Breakpoint, state_.pc, 0);
            try {
                if (decoded_.operation == core::Operation::Mret) step.next.pc = next_interrupts.mret();
                else if (decoded_.operation == core::Operation::Sret) step.next.pc = next_interrupts.sret();
                else if (decoded_.operation == core::Operation::SfenceVma) next_interrupts.check_sfence_vma();
                else if (decoded_.operation == core::Operation::Wfi) next_interrupts.wait_for_interrupt();
                else if (core::is_csr_operation(decoded_.operation)) {
                    const auto value = core::access_csr(next_interrupts, decoded_, state_.a);
                    step.next.gprs.write(decoded_.rd, value);
                    if (decoded_.rd) step.register_write = core::datapath::RV32RegisterWrite{decoded_.rd, value};
                }
                // FENCE retires after all older CPU accesses have acknowledged.
                // FENCE.I also precedes the next fetch; this write-through platform
                // updates resident I-cache lines through its physical write snoop.
            } catch (const std::invalid_argument&) {
                throw core::RV32Fault(core::RV32FaultCause::IllegalInstruction, state_.pc, state_.ir);
            }
        }
        auto next_decoded = decoded_;
        if (signals.decode) {
            try { next_decoded = decoder_.decode_rv32(step.next.ir); }
            catch (const std::invalid_argument&) {
                throw core::RV32Fault(core::RV32FaultCause::IllegalInstruction, state_.pc, step.next.ir);
            }
        }
        RV32CycleTrace trace{mode(), control_.rv32_phase(), control_.rv32_timing(), signals,
            mode() == Mode::Microprogrammed
                ? std::optional<core::control_unit::microprogrammed::MicroAddress>(control_.microprogrammed().micro_pc())
                : std::nullopt,
            step.alu, signals.retire};
        if (signals.muldiv_start || signals.muldiv_step) trace.muldiv = step.next.muldiv;
        const RV32RetirementTrace retired{completed_instructions() + 1, clock_pulses() + 1,
            clock_pulses() + 1 - last_retirement_pulse_, state_.pc, step.next.pc, state_.ir,
            mode(), step.register_write, step.memory_write};

        // Local writes commit after control validation. External writes commit on
        // the bus data phase; the CPU retires only after their acknowledgement.
        control_.clock(next_decoded, {step.next.equal, step.next.muldiv.busy(), step.next.muldiv.done()});
        if (signals.system && decoded_.operation == core::Operation::SfenceVma) {
            mmu_.flush_tlb(); // Teaching format has no ASID: retain its conservative full fence.
            sv32_.flush(decoded_.rs1 ? std::optional<std::uint32_t>(state_.a) : std::nullopt,
                        decoded_.rs2 ? std::optional<unsigned>(state_.b & 511u) : std::nullopt);
        }
        if (step.memory_write && !bus_)
            memory_.write(step.memory_write->address, step.memory_write->value, step.memory_write->bytes);
        state_ = step.next;
        interrupts_ = next_interrupts;
        decoded_ = next_decoded;
        last_cycle_ = trace;
        if (signals.retire) {
            last_retirement_ = retired;
            last_retirement_pulse_ = clock_pulses();
        }
    }

public:
    RV32RetirementTrace step_instruction(std::uint64_t cycle_limit = 128) {
        const auto before = completed_instructions();
        for (std::uint64_t i = 0; i < cycle_limit; ++i) {
            tick();
            if (completed_instructions() != before) return *last_retirement_;
        }
        throw std::runtime_error("RV32 instruction cycle limit exceeded");
    }
    void run_instructions(std::uint64_t count, std::uint64_t cycle_limit = 128) {
        for (std::uint64_t i = 0; i < count; ++i) step_instruction(cycle_limit);
    }

    Mode mode() const { return control_.mode(); }
    Execution execution() const { return execution_; }
    void select_mode(Mode mode) {
        if (execution_ == Execution::Pipelined && mode != Mode::Hardwired)
            throw std::invalid_argument("microprograms require MultiCycle execution");
        control_.select_mode(mode);
    }
    std::uint32_t pc() const { return state_.pc; }
    std::uint64_t clock_pulses() const {
        return execution_ == Execution::Pipelined ? pipeline_.clock_pulses() : control_.rv32_clock_pulses();
    }
    std::uint64_t completed_instructions() const {
        return execution_ == Execution::Pipelined ? pipeline_.completed_instructions() : control_.rv32_completed_instructions();
    }
    const core::datapath::dedicated::RegisterFile& registers() const { return state_.gprs; }
    const core::datapath::dedicated::DataMemory& memory() const { return memory_; }
    std::size_t data_words() const { return data_words_; }
    const core::datapath::RV32State& state() const { return state_; }
    const core::ControlUnit& control_unit() const { return control_; }
    const std::optional<RV32CycleTrace>& last_cycle() const { return last_cycle_; }
    const std::optional<RV32RetirementTrace>& last_retirement() const { return last_retirement_; }
    const std::optional<core::pipeline::CycleTrace>& last_pipeline_cycle() const { return last_pipeline_cycle_; }
    const core::pipeline::Pipeline& pipeline() const { return pipeline_; }
    // 宿主初始化接口，与控制器发出的寄存器/内存写使能分开。
    void set_register(std::size_t index, std::uint32_t value) { state_.gprs.write(index, value); }
    void set_memory(std::uint32_t address, std::uint32_t value) { memory_.write(address, value); }
    void connect_bus(devices::common::DmaBus& bus) {
        if (execution_ == Execution::Pipelined ? !pipeline_.empty() : !control_.at_instruction_boundary())
            throw std::logic_error("connect CPU bus only while execution is quiescent");
        bus_ = &bus;
    }
    // Optional shared-memory instruction path. Existing ROM/pipeline behavior is unchanged.
    void connect_instruction_bus(devices::common::DmaBus& bus) {
        if (execution_ != Execution::MultiCycle || !control_.at_instruction_boundary())
            throw std::logic_error("instruction bus requires a quiescent multicycle core");
        instruction_bus_ = &bus;
    }
    void set_boot_pc(std::uint32_t address) {
        if (clock_pulses() || address % 4) throw std::logic_error("boot PC requires aligned address before first clock");
        state_.pc = address;
    }
    // This host-only teaching object is ignored by Architectural execution.
    core::MMU& mmu() { return mmu_; }
    Translation translation_model() const { return translation_; }
    const core::Sv32& sv32() const { return sv32_; }
    core::InterruptSystem& interrupts() { return interrupts_; }
    const core::InterruptSystem& interrupts() const { return interrupts_; }
    core::FaultHandling fault_handling() const { return faults_; }
    bool waiting() const { return interrupts_.waiting(); }
    std::uint64_t wfi_wait_cycles() const { return wfi_wait_cycles_; }
    std::uint64_t bus_waits() const {
        return execution_ == Execution::Pipelined ? pipeline_.bus_waits() : bus_waits_;
    }
private:
    devices::common::DmaBus* instruction_bus_ = nullptr;
    std::optional<devices::common::Tag> pending_instruction_bus_;
    void tick_pipeline() {
        const auto trace = pipeline_.tick(state_, program_, memory_, mmu_, interrupts_, bus_, faults_);
        record_pipeline_cycle(trace);
    }
    void record_pipeline_cycle(const core::pipeline::CycleTrace& trace) {
        RV32CycleTrace cycle{};
        cycle.mode = Mode::Hardwired;
        cycle.timing.clock_pulse = clock_pulses();
        cycle.timing.instruction_cycle = completed_instructions();
        cycle.instruction_complete = bool(trace.retirement);
        cycle.bus_wait = trace.memory_stall;
        cycle.interrupt_entry = trace.interrupt_entry;
        cycle.execution = execution_;
        cycle.trap = trace.trap;
        cycle.wfi_wait = trace.wfi_wait;
        cycle.muldiv = trace.muldiv;
        last_cycle_ = cycle;
        last_pipeline_cycle_ = trace;
        if (trace.retirement) {
            const auto& retired = *trace.retirement;
            last_retirement_ = RV32RetirementTrace{completed_instructions(), clock_pulses(),
                clock_pulses() - last_retirement_pulse_, retired.pc, retired.next_pc,
                retired.instruction, Mode::Hardwired, retired.register_write, retired.memory_write};
            last_retirement_pulse_ = clock_pulses();
        }
    }
    void enter_exception(const core::RV32Fault& fault) {
        RV32CycleTrace trace{};
        trace.mode = mode(); trace.phase = control_.rv32_phase(); trace.timing = control_.rv32_timing();
        if (mode() == Mode::Microprogrammed) trace.micro_address = control_.microprogrammed().micro_pc();
        const auto target = interrupts_.exception(fault.pc(), static_cast<std::uint32_t>(fault.cause()), fault.value());
        trace.trap = core::RV32Trap{fault.cause(), fault.pc(), fault.value(), target};
        control_.abort_instruction();
        state_.pc = target;
        state_.pending_instruction.reset(); state_.pending_data.reset();
        state_.muldiv.reset();
        pending_instruction_bus_.reset(); pending_bus_.reset(); sv32_.cancel();
        decoded_ = {};
        last_cycle_ = trace;
    }
    void record_hold(const core::control_unit::rv32::ControlSignals& signals, bool interrupt = false) {
        last_cycle_ = RV32CycleTrace{mode(), control_.rv32_phase(), control_.rv32_timing(), signals,
            mode() == Mode::Microprogrammed
                ? std::optional<core::control_unit::microprogrammed::MicroAddress>(control_.microprogrammed().micro_pc())
                : std::nullopt,
            std::nullopt, false, !interrupt, interrupt};
    }
    [[noreturn]] void throw_access(bool store) const {
        throw core::RV32Fault(store ? core::RV32FaultCause::StoreAccessFault : core::RV32FaultCause::LoadAccessFault,
                              state_.pc, state_.mar);
    }
    std::optional<std::uint32_t> translate_address(std::uint32_t address, unsigned bytes, core::mmu::AccessType access) {
        if (translation_ == Translation::Architectural || interrupts_.sv32_enabled(access))
            return sv32_.translate(address, bytes, access, state_.pc, interrupts_, bus_, memory_);
        // Preserve the pre-existing software-driven teaching MMU in Bare mode.
        const auto spans = mmu_.translate_range(address, bytes, access,
            interrupts_.translation_privilege(access == core::mmu::AccessType::Execute));
        for (const auto& span : spans)
            interrupts_.check_memory(span.physical_address, static_cast<unsigned>(span.size), access, state_.pc, span.virtual_address);
        return spans.at(0).physical_address;
    }
    core::Sv32 sv32_;
    devices::common::DmaBus* bus_ = nullptr;
    std::optional<devices::common::Tag> pending_bus_;
    core::InterruptSystem interrupts_;
    core::MMU mmu_;
    std::uint64_t bus_waits_ = 0;
    core::datapath::dedicated::InstructionMemory program_;
    core::datapath::dedicated::DataMemory memory_;
    core::datapath::RV32State state_;
    core::datapath::RV32Datapath datapath_;
    core::Decoder decoder_;
    core::DecodedInstruction decoded_;
    core::ControlUnit control_;
    std::size_t data_words_;
    std::uint64_t last_retirement_pulse_ = 0;
    std::optional<RV32CycleTrace> last_cycle_;
    std::optional<RV32RetirementTrace> last_retirement_;
    Execution execution_;
    core::pipeline::Pipeline pipeline_;
    std::optional<core::pipeline::CycleTrace> last_pipeline_cycle_;
    core::FaultHandling faults_;
    std::uint64_t wfi_wait_cycles_ = 0;
    Translation translation_;
};
} // namespace colab::soc
