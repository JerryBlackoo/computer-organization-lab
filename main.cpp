#include "simulation/simulator.hpp"
#include "soc/cpu16/core.hpp"

#include <iomanip>
#include <iostream>
#include <string>

namespace {
std::string signal_names(const colab::soc::core::control_unit::ControlSignals& signals) {
    using colab::soc::core::datapath::BusSource;
    using namespace colab::soc::core::control_unit;
    std::string names;
    switch (signals.bus_source) {
    case BusSource::None: break;
    case BusSource::PC: names += "PCout "; break;
    case BusSource::MDR: names += "MDRout "; break;
    case BusSource::Z: names += "Zout "; break;
    case BusSource::GPR:
        names += signals.register_select == RegisterSelect::Rs1 ? "GPRout(rs1) " : "GPRout(rs2) ";
        break;
    }
    if (signals.mar_in) names += "MARin ";
    if (signals.pc_in) names += "PCin ";
    if (signals.mdr_in) names += "MDRin ";
    if (signals.ir_in) names += "IRin ";
    if (signals.y_in) names += "Yin ";
    if (signals.z_in) names += "Zin ";
    if (signals.gpr_in) names += "GPRin(rd) ";
    if (signals.fr_in) names += "FRin ";
    if (signals.read) names += "Read ";
    if (signals.write) names += "Write ";
    if (signals.memory_to_mdr) names += "Memory->MDR ";
    if (signals.increment_pc) names += "PC+2 ";
    if (signals.decode) names += "Decode ";
    if (signals.halt) names += "HALT ";
    return names.empty() ? "-" : names;
}

void print_word(std::uint16_t value) {
    std::cout << "0x" << std::hex << std::setfill('0') << std::setw(4) << value
              << std::dec << std::setfill(' ');
}
} // namespace

int main() {
    using colab::isa::Opcode;
    // ADD:  32767 + 1 -> 0x8000, SF=1, OF=1.
    // SUB: -32768 - 1 -> 0x7FFF, SF=0, OF=1.
    colab::soc::Core core({{Opcode::Add, 3, 1, 2}, {Opcode::Sub, 4, 5, 2}, {Opcode::Halt}});
    core.set_register(1, 0x7FFF);
    core.set_register(2, 1);
    core.set_register(5, 0x8000);
    colab::simulation::Simulator<colab::soc::Core> simulator(core);

    while (!core.is_halted()) {
        simulator.step();
        const auto& trace = core.last_cycle().value();
        const auto& registers = core.registers();
        const auto flags = registers.fr.read();
        std::cout << "pulse " << std::setw(2) << trace.timing.clock_pulse
                  << " instr " << trace.timing.instruction_cycle
                  << "/T" << trace.timing.instruction_beat << ' '
                  << colab::soc::core::control_unit::stage_name(trace.timing.stage)
                  << "/T" << trace.timing.stage_beat << ' '
                  << std::left << std::setw(15) << colab::soc::core::control_unit::phase_name(trace.phase)
                  << std::setw(35) << signal_names(trace.signals) << std::right << " bus=";
        if (trace.bus) print_word(*trace.bus);
        else std::cout << "------";
        std::cout << " PC=";
        print_word(registers.pc.read());
        std::cout << " MAR=";
        print_word(registers.mar.read());
        std::cout << " MDR=";
        print_word(registers.mdr.read());
        std::cout << " IR=";
        print_word(registers.ir.read());
        std::cout << " Y=";
        print_word(registers.y.read());
        std::cout << " Z=";
        print_word(registers.z.read());
        std::cout << " SF=" << flags.sf << " OF=" << flags.of
                  << " CF=" << flags.cf << " ZF=" << flags.zf;
        if (trace.alu) {
            std::cout << (trace.signals.alu_op == colab::soc::core::ALUOperation::Add ? " ADD" : " SUB");
            std::cout << " A15=" << (registers.y.read() >> 15)
                      << " B15=" << (*trace.bus >> 15)
                      << " F15=" << (trace.alu->value >> 15);
        }
        if (trace.instruction_complete) std::cout << " [instruction complete]";
        else if (trace.stage_complete) {
            using namespace colab::soc::core::control_unit;
            std::cout << " [" << stage_name(trace.timing.stage) << " -> "
                      << stage_name(instruction_stage(trace.next_phase)) << ']';
        }
        std::cout << '\n';
    }

    std::cout << "Final: R3=";
    print_word(core.registers().gprs.read(3));
    std::cout << " R4=";
    print_word(core.registers().gprs.read(4));
    std::cout << " cycles=" << simulator.cycles()
              << " instructions=" << core.completed_instructions() << '\n';
}
