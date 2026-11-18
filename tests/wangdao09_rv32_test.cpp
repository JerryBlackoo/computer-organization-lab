#include "../soc/rv32/core.hpp"
#include <cassert>
using CPU = colab::soc::RV32Core;
namespace pipe = colab::soc::core::pipeline;
int main() {
    for (const bool enabled : {false, true}) {
        CPU cpu({0x00110113, 0x0001d283, 0x40520133}, CPU::Mode::Hardwired,
                256, {}, CPU::Execution::Pipelined, {enabled});
        cpu.set_register(2, 5); cpu.set_register(3, 100); cpu.set_register(4, 20);
        cpu.set_memory(100, 6);
        unsigned stalls = 0; bool forwarded = false, increment_retired = false;
        while (cpu.completed_instructions() < 3 && cpu.clock_pulses() < 30) {
            cpu.tick();
            const auto& t = *cpu.last_pipeline_cycle();
            stalls += t.data_stall;
            forwarded |= t.forward_b == pipe::Forward::MemWb;
            if (t.retirement && t.retirement->pc == 0) {
                assert(t.retirement->register_write && t.retirement->register_write->value == 6);
                increment_retired = true;
            }
        }
        assert(increment_retired && cpu.completed_instructions() == 3 && cpu.registers().read(2) == 14);
        assert(stalls == (enabled ? 1u : 2u));
        assert(forwarded == enabled);
        assert(cpu.clock_pulses() == (enabled ? 8u : 9u));
    }
}
