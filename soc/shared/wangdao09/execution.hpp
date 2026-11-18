#pragma once

#include "../../cpu16/datapath.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>

// Execution-only model for the 2015 exercise. Fetch/decode timing is excluded.
// It shares the real 16-bit single-bus datapath, not the RV32 ISA.
namespace colab::soc::core::wangdao09 {
class Execution {
public:
    enum class Bypass { Disabled, Enabled };
    enum class Phase { Idle, IncInput, IncAlu, IncWrite, SubAddress, SubOperand, SubAlu, SubWrite };
    struct Trace {
        Phase phase;
        control_unit::ControlSignals signals;
        std::optional<std::uint16_t> bus;
        std::optional<ALUResult> alu;
        std::optional<std::uint16_t> memory_address;
    };
    explicit Execution(Bypass bypass) : bypass_(bypass) {}

    // Host initializes registers and a little-endian byte-addressed memory.
    void set_register(unsigned n, std::uint16_t value) { check_register(n); registers_.gprs.write(n, value); }
    std::uint16_t reg(unsigned n) const { check_register(n); return registers_.gprs.read(n); }
    void set_memory16(std::uint16_t address, std::uint16_t value) {
        check_address(address);
        memory_[address] = static_cast<std::uint8_t>(value);
        memory_[address + 1u] = static_cast<std::uint8_t>(value >> 8);
    }
    // Accept only inc Rd and sub Rd,Rs1,(Rs2), with unused/mode bits validated.
    void begin(std::uint16_t word) {
        if (phase_ != Phase::Idle) throw std::logic_error("instruction still executing");
        const auto op = word >> 9;
        const auto md = (word >> 8) & 1u;
        const auto ms1 = (word >> 5) & 1u;
        const auto ms2 = (word >> 2) & 1u;
        const auto rd = (word >> 6) & 3u;
        const auto rs1 = (word >> 3) & 3u;
        const auto rs2 = word & 3u;
        Phase first;
        if (op == 1 && !md && (word & 0x3fu) == 0) first = Phase::IncInput;
        else if (op == 3 && !md && !ms1 && ms2) first = Phase::SubAddress;
        else throw std::invalid_argument("unsupported Wangdao 09 instruction/addressing mode");
        rd_ = rd; rs1_ = rs1; rs2_ = rs2; phase_ = first; cycles_ = 0; last_.reset();
    }
    void tick() {
        using control_unit::ControlSignals;
        using datapath::BusSource;
        if (phase_ == Phase::Idle) throw std::logic_error("no active instruction");
        ControlSignals s;
        unsigned read = 0;
        Phase next = Phase::Idle;
        std::optional<std::uint16_t> response;
        std::optional<std::uint16_t> requested_address;
        switch (phase_) {
        case Phase::IncInput:
            s.bus_source = BusSource::GPR; read = rd_; s.y_in = true;
            if (bypass_ == Bypass::Enabled) {
                s.alu_a_from_bus = true; s.alu_op = ALUOperation::Increment; s.z_in = true;
                next = Phase::IncWrite;
            } else next = Phase::IncAlu;
            break;
        case Phase::IncAlu:
            s.alu_op = ALUOperation::Increment; s.z_in = true; next = Phase::IncWrite; break;
        case Phase::IncWrite:
            s.bus_source = BusSource::Z; s.gpr_in = true; break;
        case Phase::SubAddress:
            s.bus_source = BusSource::GPR; read = rs2_; s.mar_in = true;
            // Explicit address bypass: memory request samples the same old GPR
            // that drives MAR, rather than reading MAR before its clock edge.
            requested_address = reg(rs2_); check_address(*requested_address);
            next = Phase::SubOperand; break;
        case Phase::SubOperand:
            if (!pending_) throw std::logic_error("memory response missing");
            s.bus_source = BusSource::GPR; read = rs1_; s.y_in = true;
            s.read = s.mdr_in = s.memory_to_mdr = true; response = pending_;
            next = Phase::SubAlu; break;
        case Phase::SubAlu:
            s.bus_source = BusSource::MDR; s.alu_op = ALUOperation::Subtract;
            s.z_in = true; next = Phase::SubWrite; break;
        case Phase::SubWrite:
            s.bus_source = BusSource::Z; s.gpr_in = true; break;
        case Phase::Idle: throw std::logic_error("no active instruction");
        }
        const auto step = datapath_.evaluate(registers_, s, read, rd_, response);
        auto next_pending = pending_;
        if (requested_address) {
            const auto address = *requested_address;
            next_pending = static_cast<std::uint16_t>(memory_[address] | (unsigned(memory_[address + 1u]) << 8));
        }
        if (response) next_pending.reset();
        last_ = Trace{phase_, s, step.bus, step.alu, requested_address};
        registers_ = step.next_registers;
        pending_ = next_pending;
        phase_ = next;
        ++cycles_;
    }
    bool done() const { return phase_ == Phase::Idle; }
    unsigned cycles() const { return cycles_; }
    const std::optional<Trace>& last_cycle() const { return last_; }
    const Registers& registers() const { return registers_; }
private:
    static void check_register(unsigned n) { if (n >= 4) throw std::out_of_range("R0-R3 only"); }
    static void check_address(std::uint16_t n) { if (n == 0xffffu) throw std::out_of_range("16-bit read exceeds memory"); }
    Bypass bypass_;
    Phase phase_ = Phase::Idle;
    unsigned rd_ = 0, rs1_ = 0, rs2_ = 0, cycles_ = 0;
    Registers registers_;
    datapath::SingleBusDatapath datapath_;
    std::array<std::uint8_t, 65536> memory_{};
    std::optional<std::uint16_t> pending_;
    std::optional<Trace> last_;
};
}
