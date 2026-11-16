#include "../../soc/soc.hpp"

#include <array>
#include <iostream>

int main() {
    using namespace colab;
    using CPU = soc::RV32Core;
    struct Configuration { const char* name; CPU::Execution execution; CPU::Mode mode; };
    const std::array<Configuration, 3> configurations{{
        {"RV32 pipeline", CPU::Execution::Pipelined, CPU::Mode::Hardwired},
        {"Hardwired multi-cycle", CPU::Execution::MultiCycle, CPU::Mode::Hardwired},
        {"Microprogrammed multi-cycle", CPU::Execution::MultiCycle, CPU::Mode::Microprogrammed}
    }};
    // LW x3,0(x1); LW x4,0(x1); SW x2,0(x1); LW x5,0(x1).
    for (const auto& configuration : configurations) {
        SoCConfig config; config.cpu_mode = configuration.mode;
        config.cpu_execution = configuration.execution;
        SoC system({0x0000A183, 0x0000A203, 0x0020A023, 0x0000A283}, config);
        system.cpu().set_register(1, 0x1000); system.cpu().set_register(2, 42);
        system.memory().write(0x1000, devices::common::word_bytes(7));
        std::cout << configuration.name << '\n';
        const char* actions[]{"LW cold", "LW same word", "SW write-through", "LW updated word"};
        for (unsigned instruction = 0; instruction < 4; ++instruction) {
            const auto before = system.cycles();
            const auto reads = system.memory().statistics().read_requests;
            const auto transactions = system.bus().fabric().statistics().completed;
            while (system.cpu().completed_instructions() <= instruction && system.cycles() - before < 10000)
                system.tick();
            if (system.cpu().completed_instructions() <= instruction) throw std::runtime_error("CPU Cache demo timed out");
            const auto& stats = system.data_cache().statistics();
            std::cout << "  " << actions[instruction] << ": cycles=" << system.cycles() - before
                      << " bus-transactions=" << system.bus().fabric().statistics().completed - transactions
                      << " DIMM-reads=" << system.memory().statistics().read_requests - reads
                      << " cache-hits=" << stats.hits() << " cache-misses=" << stats.misses() << '\n';
        }
        if (system.cpu().registers().read(3) != 7 || system.cpu().registers().read(4) != 7 ||
            system.cpu().registers().read(5) != 42 ||
            devices::common::load32(system.memory().read(0x1000, 4)) != 42)
            throw std::runtime_error("CPU Cache demo result mismatch");
        std::cout << "  x3=7 x4=7 x5=42 DIMM=42\n";
    }
}
