#pragma once

#include "../datapath/rv32.hpp"
#include "../datapath/dedicated/rv32_alu_control.hpp"
#include "../../shared/decoder/decoder.hpp"
#include "../interrupt_system/interrupt_system.hpp"
#include "../csr/csr_access.hpp"
#include "../../shared/mmu/mmu.hpp"
#include "../../shared/mmu/rv32_fault.hpp"
#include "../../../devices/common/dma_bus.hpp"

#include <array>
#include <exception>

namespace colab::soc::core::pipeline {

enum class Execution { MultiCycle, Pipelined };
enum class Forward { Register, ExMem, MemWb };
struct Options { bool forwarding = true; };

// Each valid latch belongs to one instruction. A bubble has no architectural effect.
struct Latch {
    bool valid = false;
    std::uint32_t pc = 0, instruction = 0, next_pc = 0;
    DecodedInstruction decoded{};
    std::uint32_t a = 0, b = 0, value = 0, address = 0;
    std::exception_ptr fault;
    std::optional<devices::common::Tag> request;
    // Once submitted, a bus transaction (including an MMIO read) cannot be squashed.
    bool external_effect = false;
};

struct Retirement {
    std::uint32_t pc, next_pc, instruction;
    std::optional<datapath::RV32RegisterWrite> register_write;
    std::optional<datapath::RV32MemoryWrite> memory_write;
};

struct CycleTrace {
    std::uint64_t pulse = 0;
    // Occupancy at the beginning of the cycle: IF, ID, EX, MEM, WB.
    std::array<std::optional<std::uint32_t>, 5> pc{};
    Forward forward_a = Forward::Register, forward_b = Forward::Register;
    bool data_stall = false, memory_stall = false, system_stall = false, muldiv_stall = false;
    bool interrupt_entry = false, redirected = false;
    unsigned flushed = 0;
    std::uint32_t target = 0;
    std::optional<Retirement> retirement;
    std::optional<RV32Trap> trap{};
    bool wfi_wait = false;
    std::optional<datapath::dedicated::MulDiv> muldiv{};
};

// RV32 execution engine owned by RV32Core. Architectural state, MMU, memory,
// interrupt CSRs and the asynchronous data port are the existing core's objects.
class Pipeline {
public:
    explicit Pipeline(Options options = {}) : options_(options) {}

    CycleTrace tick(datapath::RV32State& state,
                    const datapath::dedicated::InstructionMemory& program,
                    datapath::dedicated::DataMemory& memory, MMU& mmu,
                    InterruptSystem& interrupts, devices::common::DmaBus* bus,
                    FaultHandling handling = FaultHandling::HostException) {
        if (!initialized_) { fetch_pc_ = state.pc; initialized_ = true; }
        CycleTrace trace;
        trace.pulse = pulses_ + 1;
        trace.pc[0] = fetch_pc_;
        const std::array<const Latch*, 4> old{{&if_id_, &id_ex_, &ex_mem_, &mem_wb_}};
        for (unsigned n = 0; n < old.size(); ++n)
            if (old[n]->valid) trace.pc[n + 1] = old[n]->pc;

        // An older synchronous fault wins over a younger memory request or IRQ.
        if (mem_wb_.valid && mem_wb_.fault)
            return fault_cycle(mem_wb_.fault, state, interrupts, std::move(trace), handling);

        if (interrupts.requested()) {
            if (ex_mem_.request || (mem_wb_.valid && mem_wb_.external_effect)) {
                draining_irq_ = true;
            } else {
                auto next_interrupts = interrupts;
                const auto target = next_interrupts.enter(state.pc);
                trace.flushed = occupied();
                reset_frontend(target);
                state.pc = target;
                interrupts = next_interrupts;
                trace.interrupt_entry = true;
                trace.redirected = true;
                trace.target = target;
                return finish(std::move(trace));
            }
        }

        if (mem_wb_.valid) {
            try { trace.retirement = retire(mem_wb_, state, memory, interrupts); }
            catch (...) { return fault_cycle(std::current_exception(), state, interrupts, std::move(trace), handling); }
            ++retired_;
        }

        // CSRs/MRET serialize execution. No younger memory request is issued in
        // their retirement cycle; discard speculation and restart at committed PC.
        if (mem_wb_.valid && system(mem_wb_.decoded.operation)) {
            if (mem_wb_.decoded.operation == Operation::SfenceVma) mmu.flush_tlb();
            trace.flushed = unsigned(if_id_.valid) + unsigned(id_ex_.valid) + unsigned(ex_mem_.valid);
            reset_frontend(state.pc);
            trace.redirected = true;
            trace.target = state.pc;
            return finish(std::move(trace));
        }

        if (draining_irq_) {
            // Preserve only the already submitted transaction and its retirement.
            // If the IRQ level disappears, restart squashed instructions at state.pc.
            trace.flushed = unsigned(if_id_.valid) + unsigned(id_ex_.valid);
            if_id_ = {}; id_ex_ = {}; mem_wb_ = {};
            muldiv_.reset();
            if (ex_mem_.request) {
                auto pending = ex_mem_;
                if (access(pending, memory, mmu, bus, interrupts)) { mem_wb_ = pending; ex_mem_ = {}; }
                else { ex_mem_ = pending; trace.memory_stall = true; ++bus_waits_; }
            } else { trace.flushed += unsigned(ex_mem_.valid); ex_mem_ = {}; }
            if (!ex_mem_.valid && !mem_wb_.valid) {
                draining_irq_ = false;
                fetch_pc_ = state.pc;
            }
            return finish(std::move(trace));
        }

        Latch next_wb = ex_mem_;
        if (!access(next_wb, memory, mmu, bus, interrupts)) {
            // WB may drain during a MEM wait. Save its forwarding value before
            // removing it, otherwise the held EX instruction could lose a producer.
            if (options_.forwarding && id_ex_.valid && !id_ex_.fault) {
                if (uses_a(id_ex_.decoded.operation))
                    id_ex_.a = operand(id_ex_.decoded.rs1, id_ex_.a, trace.forward_a);
                if (uses_b(id_ex_.decoded.operation))
                    id_ex_.b = operand(id_ex_.decoded.rs2, id_ex_.b, trace.forward_b);
            }
            ex_mem_ = next_wb;
            mem_wb_ = {};
            trace.memory_stall = true;
            ++bus_waits_;
            return finish(std::move(trace));
        }

        Latch next_mem;
        if (id_ex_.valid && !id_ex_.fault && is_muldiv(id_ex_.decoded.operation)) {
            if (!muldiv_.busy()) {
                // Capture forwarded sources once. Producers can retire while EX is held.
                if (options_.forwarding) {
                    id_ex_.a = operand(id_ex_.decoded.rs1, id_ex_.a, trace.forward_a);
                    id_ex_.b = operand(id_ex_.decoded.rs2, id_ex_.b, trace.forward_b);
                }
                muldiv_.start(datapath::dedicated::rv32_alu_control(id_ex_.decoded.operation), id_ex_.a, id_ex_.b);
            } else muldiv_.step(alu_);
            trace.muldiv = muldiv_;
            if (!muldiv_.done()) {
                // Drain older MEM/WB once, while keeping EX, ID and IF in place.
                mem_wb_ = next_wb; ex_mem_ = {};
                trace.muldiv_stall = true;
                return finish(std::move(trace));
            }
            next_mem = id_ex_;
            next_mem.value = muldiv_.result();
            muldiv_.reset();
        } else next_mem = execute(id_ex_, trace);
        Latch next_ex, next_id;
        auto next_pc = fetch_pc_;
        if (next_mem.valid && !next_mem.fault &&
            (is_conditional_branch(next_mem.decoded.operation) || next_mem.decoded.operation == Operation::Jal ||
             next_mem.decoded.operation == Operation::Jalr) && next_mem.next_pc != next_mem.pc + 4u) {
            trace.redirected = true;
            trace.target = next_mem.next_pc;
            trace.flushed = unsigned(if_id_.valid) + 1; // ID plus this cycle's IF opportunity.
            next_pc = next_mem.next_pc;
        } else if (system_barrier()) {
            trace.system_stall = true;
            next_id = if_id_;
        } else if (hazard()) {
            trace.data_stall = true;
            next_id = if_id_;
        } else {
            next_ex = decode(if_id_, state.gprs);
            next_id = fetch(program, fetch_pc_, interrupts);
            next_pc += 4u;
        }

        mem_wb_ = next_wb; ex_mem_ = next_mem; id_ex_ = next_ex; if_id_ = next_id;
        fetch_pc_ = next_pc;
        return finish(std::move(trace));
    }

    std::uint64_t clock_pulses() const { return pulses_; }
    CycleTrace idle_clock() {
        CycleTrace trace; trace.pulse = pulses_ + 1; trace.wfi_wait = true;
        return finish(std::move(trace));
    }
    std::uint64_t completed_instructions() const { return retired_; }
    std::uint64_t bus_waits() const { return bus_waits_; }
    std::uint32_t fetch_pc() const { return fetch_pc_; }
    bool empty() const { return occupied() == 0; }
    const Latch& if_id() const { return if_id_; }
    const Latch& id_ex() const { return id_ex_; }
    const Latch& ex_mem() const { return ex_mem_; }
    const Latch& mem_wb() const { return mem_wb_; }
    const datapath::dedicated::MulDiv& muldiv() const { return muldiv_; }

private:
    static bool system(Operation op) {
        return is_system_operation(op);
    }
    static bool uses_a(Operation op) {
        return !is_csr_immediate(op) && !is_fence(op) && op != Operation::Mret && op != Operation::Sret &&
            op != Operation::SfenceVma && op != Operation::Wfi && op != Operation::Ecall && op != Operation::Ebreak &&
            op != Operation::Lui && op != Operation::Auipc && op != Operation::Jal;
    }
    static bool uses_b(Operation op) {
        // M operations are register ALU operations too, including rs2 hazards.
        return is_register_alu(op) || is_store(op) || is_conditional_branch(op);
    }
    static bool writes(Operation op) {
        return !is_store(op) && !is_fence(op) && !is_conditional_branch(op) && op != Operation::Mret &&
            op != Operation::Sret && op != Operation::SfenceVma && op != Operation::Wfi &&
            op != Operation::Ecall && op != Operation::Ebreak;
    }
    static bool matches(const Latch& producer, std::uint8_t source) {
        return source && producer.valid && !producer.fault && writes(producer.decoded.operation) &&
            producer.decoded.rd == source;
    }
    static Latch fetch(const datapath::dedicated::InstructionMemory& program, std::uint32_t pc, const InterruptSystem& interrupts) {
        Latch slot;
        slot.valid = true; slot.pc = pc; slot.next_pc = pc + 4u;
        try {
            if (pc % 4) throw RV32Fault(RV32FaultCause::InstructionAddressMisaligned, pc, pc);
            interrupts.check_memory(pc, 4, mmu::AccessType::Execute, pc);
            try { slot.instruction = program.read(pc); }
            catch (const std::out_of_range&) { throw RV32Fault(RV32FaultCause::InstructionAccessFault, pc, pc); }
        } catch (...) { slot.fault = std::current_exception(); }
        return slot;
    }
    static Latch decode(Latch slot, const datapath::dedicated::RegisterFile& registers) {
        if (!slot.valid || slot.fault) return slot;
        try {
            try { slot.decoded = Decoder{}.decode_rv32(slot.instruction); }
            catch (const std::invalid_argument&) {
                throw RV32Fault(RV32FaultCause::IllegalInstruction, slot.pc, slot.instruction);
            }
            const auto ports = registers.read(slot.decoded.rs1, slot.decoded.rs2);
            slot.a = ports.bus_a; slot.b = ports.bus_b;
        } catch (...) { slot.fault = std::current_exception(); }
        return slot;
    }
    bool system_barrier() const {
        return (id_ex_.valid && !id_ex_.fault && system(id_ex_.decoded.operation)) ||
               (ex_mem_.valid && !ex_mem_.fault && system(ex_mem_.decoded.operation));
    }
    bool hazard() const {
        const auto consumer = decode(if_id_, datapath::dedicated::RegisterFile{});
        if (!consumer.valid || consumer.fault) return false;
        const auto depends = [&](const Latch& producer) {
            return (uses_a(consumer.decoded.operation) && matches(producer, consumer.decoded.rs1)) ||
                   (uses_b(consumer.decoded.operation) && matches(producer, consumer.decoded.rs2));
        };
        if (options_.forwarding) return is_load(id_ex_.decoded.operation) && depends(id_ex_);
        return depends(id_ex_) || depends(ex_mem_);
    }
    std::uint32_t operand(std::uint8_t source, std::uint32_t value, Forward& route) const {
        if (matches(ex_mem_, source) && !is_load(ex_mem_.decoded.operation) &&
            !system(ex_mem_.decoded.operation)) {
            route = Forward::ExMem; return ex_mem_.value;
        }
        if (matches(mem_wb_, source) && !system(mem_wb_.decoded.operation)) {
            route = Forward::MemWb; return mem_wb_.value;
        }
        return value;
    }
    Latch execute(Latch slot, CycleTrace& trace) const {
        if (!slot.valid || slot.fault) return slot;
        try {
            const auto& instruction = slot.decoded;
            if (options_.forwarding) {
                if (uses_a(instruction.operation)) slot.a = operand(instruction.rs1, slot.a, trace.forward_a);
                if (uses_b(instruction.operation)) slot.b = operand(instruction.rs2, slot.b, trace.forward_b);
            }
            if (system(instruction.operation)) return slot; // CSR state changes only at WB.
            if (is_conditional_branch(instruction.operation)) {
                const bool less_signed = (slot.a ^ 0x80000000u) < (slot.b ^ 0x80000000u);
                bool taken = false;
                switch (instruction.operation) {
                case Operation::Beq: taken = slot.a == slot.b; break;
                case Operation::Bne: taken = slot.a != slot.b; break;
                case Operation::Blt: taken = less_signed; break;
                case Operation::Bge: taken = !less_signed; break;
                case Operation::Bltu: taken = slot.a < slot.b; break;
                case Operation::Bgeu: taken = slot.a >= slot.b; break;
                default: break;
                }
                if (taken) {
                    slot.next_pc = slot.pc + static_cast<std::uint32_t>(instruction.immediate);
                    if (slot.next_pc % 4)
                        throw RV32Fault(RV32FaultCause::InstructionAddressMisaligned, slot.pc, slot.next_pc);
                }
                return slot;
            }
            if (instruction.operation == Operation::Lui || instruction.operation == Operation::Auipc) {
                slot.value = static_cast<std::uint32_t>(instruction.immediate) +
                    (instruction.operation == Operation::Auipc ? slot.pc : 0u);
                return slot;
            }
            if (instruction.operation == Operation::Jal || instruction.operation == Operation::Jalr) {
                slot.value = slot.pc + 4u;
                slot.next_pc = instruction.operation == Operation::Jal
                    ? slot.pc + static_cast<std::uint32_t>(instruction.immediate)
                    : (slot.a + static_cast<std::uint32_t>(instruction.immediate)) & ~1u;
                if (slot.next_pc % 4)
                    throw RV32Fault(RV32FaultCause::InstructionAddressMisaligned, slot.pc, slot.next_pc);
                return slot;
            }
            const bool immediate = is_alu_immediate(instruction.operation) || is_load(instruction.operation) ||
                is_store(instruction.operation);
            const auto result = alu_.evaluate(datapath::dedicated::rv32_alu_control(instruction.operation),
                slot.a, immediate ? static_cast<std::uint32_t>(instruction.immediate) : slot.b);
            slot.value = result.result;
            slot.address = result.result;
        } catch (...) { slot.fault = std::current_exception(); }
        return slot;
    }
    // Called only after the older WB instruction has retired. Thus even reads
    // from destructive MMIO registers are never speculative.
    static bool access(Latch& slot, datapath::dedicated::DataMemory& memory, MMU& mmu,
                       devices::common::DmaBus* bus, const InterruptSystem& interrupts) {
        if (!slot.valid || slot.fault) return true;
        const auto op = slot.decoded.operation;
        if (!is_load(op) && !is_store(op)) return true;
        const bool store = is_store(op);
        const auto bytes = memory_access_bytes(op);
        try {
            if (slot.address % bytes) throw RV32Fault(store ? RV32FaultCause::StoreAddressMisaligned :
                RV32FaultCause::LoadAddressMisaligned, slot.pc, slot.address);
            if (!bus) {
                interrupts.check_memory(slot.address, bytes, store ? mmu::AccessType::Write : mmu::AccessType::Read, slot.pc);
                try {
                    memory.validate(slot.address, bytes);
                    if (!store) slot.value = datapath::dedicated::extend_load(memory.read(slot.address, bytes), bytes, signed_load(op));
                } catch (const std::out_of_range&) {
                    throw RV32Fault(store ? RV32FaultCause::StoreAccessFault : RV32FaultCause::LoadAccessFault,
                        slot.pc, slot.address);
                }
                return true;
            }
            if (!slot.request) {
                const auto spans = mmu.translate_range(slot.address, bytes,
                    store ? mmu::AccessType::Write : mmu::AccessType::Read, interrupts.translation_privilege());
                for (const auto& span : spans)
                    interrupts.check_memory(span.physical_address, static_cast<unsigned>(span.size), store
                        ? mmu::AccessType::Write : mmu::AccessType::Read, slot.pc, span.virtual_address);
                try {
                    slot.request = bus->submit(0, store ? devices::common::BusOperation::Write :
                        devices::common::BusOperation::Read, spans.at(0).physical_address, bytes,
                        store ? devices::common::scalar_bytes(slot.b, bytes) : devices::common::Bytes{});
                    slot.external_effect = true;
                } catch (const std::length_error&) { return false; }
                catch (const std::exception&) {
                    throw RV32Fault(store ? RV32FaultCause::StoreAccessFault : RV32FaultCause::LoadAccessFault,
                        slot.pc, slot.address);
                }
            }
            if (!bus->ready(*slot.request)) return false;
            const auto result = bus->take(*slot.request);
            slot.request.reset();
            if (result.status != interconnect::fabric::Status::Success || result.transferred_bytes != bytes ||
                (!store && result.data.size() != bytes))
                throw RV32Fault(store ? RV32FaultCause::StoreAccessFault : RV32FaultCause::LoadAccessFault,
                    slot.pc, slot.address);
            if (!store) slot.value = datapath::dedicated::extend_load(devices::common::load_scalar(result.data), bytes, signed_load(op));
        } catch (...) { slot.fault = std::current_exception(); }
        return true;
    }
    static Retirement retire(const Latch& slot, datapath::RV32State& state,
                             datapath::dedicated::DataMemory& memory, InterruptSystem& interrupts) {
        Retirement result{slot.pc, slot.next_pc, slot.instruction, std::nullopt, std::nullopt};
        auto value = slot.value;
        auto next_interrupts = interrupts;
        const auto op = slot.decoded.operation;
        if (op == Operation::Ecall) throw RV32Fault(interrupts.ecall_cause(), slot.pc, 0);
        if (op == Operation::Ebreak) throw RV32Fault(RV32FaultCause::Breakpoint, slot.pc, 0);
        if (system(op)) {
            try {
                if (op == Operation::Mret) result.next_pc = next_interrupts.mret();
                else if (op == Operation::Sret) result.next_pc = next_interrupts.sret();
                else if (op == Operation::SfenceVma) next_interrupts.check_sfence_vma();
                else if (op == Operation::Wfi) next_interrupts.wait_for_interrupt();
                else if (is_csr_operation(op)) value = access_csr(next_interrupts, slot.decoded, slot.a);
            } catch (const std::invalid_argument&) {
                throw RV32Fault(RV32FaultCause::IllegalInstruction, slot.pc, slot.instruction);
            }
        }
        if (is_store(op)) {
            const auto bytes = memory_access_bytes(op);
            if (!slot.external_effect) memory.write(slot.address, slot.b, bytes);
            result.memory_write = datapath::RV32MemoryWrite{slot.address, slot.b & datapath::dedicated::memory_mask(bytes), bytes};
        }
        if (writes(op) && slot.decoded.rd) {
            state.gprs.write(slot.decoded.rd, value);
            result.register_write = datapath::RV32RegisterWrite{slot.decoded.rd, value};
        }
        state.pc = result.next_pc;
        interrupts = next_interrupts;
        return result;
    }
    unsigned occupied() const {
        return unsigned(if_id_.valid) + unsigned(id_ex_.valid) + unsigned(ex_mem_.valid) + unsigned(mem_wb_.valid);
    }
    void reset_frontend(std::uint32_t pc) {
        if_id_ = {}; id_ex_ = {}; ex_mem_ = {}; mem_wb_ = {};
        muldiv_.reset();
        fetch_pc_ = pc;
        draining_irq_ = false;
    }
    [[noreturn]] void fail(std::exception_ptr fault, std::uint32_t pc) {
        reset_frontend(pc);
        std::rethrow_exception(fault);
    }
    CycleTrace fault_cycle(std::exception_ptr fault, datapath::RV32State& state,
                           InterruptSystem& interrupts, CycleTrace trace, FaultHandling handling) {
        if (handling == FaultHandling::HostException) fail(fault, state.pc);
        try { std::rethrow_exception(fault); }
        catch (const RV32Fault& error) { return trap_cycle(error, state, interrupts, std::move(trace)); }
        catch (const mmu::TranslationFault& error) {
            return trap_cycle(rv32_translation_fault(error, state.pc), state, interrupts, std::move(trace));
        }
        catch (...) { fail(fault, state.pc); }
    }
    CycleTrace trap_cycle(const RV32Fault& fault, datapath::RV32State& state,
                          InterruptSystem& interrupts, CycleTrace trace) {
        const auto target = interrupts.exception(fault.pc(), static_cast<std::uint32_t>(fault.cause()), fault.value());
        trace.trap = RV32Trap{fault.cause(), fault.pc(), fault.value(), target};
        trace.flushed = occupied();
        reset_frontend(target);
        state.pc = target;
        trace.redirected = true; trace.target = target;
        return finish(std::move(trace));
    }
    CycleTrace finish(CycleTrace trace) { ++pulses_; return trace; }

    Options options_;
    Latch if_id_, id_ex_, ex_mem_, mem_wb_;
    datapath::dedicated::MulDiv muldiv_;
    datapath::dedicated::ALU alu_;
    std::uint32_t fetch_pc_ = 0;
    bool initialized_ = false, draining_irq_ = false;
    std::uint64_t pulses_ = 0, retired_ = 0, bus_waits_ = 0;
};
} // namespace colab::soc::core::pipeline
