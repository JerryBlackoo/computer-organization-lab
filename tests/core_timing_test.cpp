#include "../simulation/simulator.hpp"
#include "../soc/cpu16/core.hpp"

#include <cassert>
#include <cstdint>
#include <vector>

using colab::isa::Opcode;
using colab::soc::Core;
using colab::soc::core::control_unit::InstructionStage;

void test_instruction_cycles() {
    Core core({{Opcode::Add, 3, 1, 2}, {Opcode::Sub, 4, 3, 2}, {Opcode::Halt}});
    core.set_register(1, 0x7FFF);
    core.set_register(2, 1);
    colab::simulation::Simulator<Core> simulator(core);

    std::uint64_t pulses = 0;
    for (std::uint64_t instruction = 1; instruction <= 3; ++instruction) {
        const bool halt = instruction == 3;
        const unsigned length = halt ? 8 : 10;
        struct StageRun { InstructionStage stage; unsigned beats; };
        std::vector<StageRun> stages{{InstructionStage::Fetch, 7}};
        if (!halt) stages.push_back({InstructionStage::Decode, 1});
        stages.push_back({InstructionStage::Execute, 1});
        if (!halt) stages.push_back({InstructionStage::WriteBack, 1});
        unsigned beat = 0;
        for (const auto& run : stages) {
            for (unsigned stage_beat = 1; stage_beat <= run.beats; ++stage_beat) {
                ++beat;
                assert(core.stage() == run.stage);
                simulator.step();
                const auto& trace = core.last_cycle().value();
                const auto& timing = trace.timing;
                assert(timing.clock_pulse == ++pulses);
                assert(timing.instruction_cycle == instruction);
                assert(timing.instruction_beat == beat);
                assert(timing.stage == run.stage && timing.stage_beat == stage_beat);
                assert(trace.signals.decode == (beat == 7));
                assert(trace.signals.ir_in == (beat == 7));
                assert(trace.signals.ir_in == trace.signals.decode);
                assert(trace.next_phase == core.phase());
                assert(trace.stage_complete == (stage_beat == run.beats));
                assert(trace.instruction_complete == (beat == length));
                assert(core.clock_pulses() == pulses && simulator.cycles() == pulses);
                assert(core.completed_instructions() == instruction - (beat < length ? 1 : 0));
                assert(core.is_halted() == (halt && beat == length));
            }
        }
        assert(beat == length);
    }
    assert(pulses == 28 && core.completed_instructions() == 3);
    assert(core.registers().gprs.read(3) == 0x8000);
    assert(core.registers().gprs.read(4) == 0x7FFF);
    core.tick();
    assert(core.clock_pulses() == 28 && core.completed_instructions() == 3);
    assert(core.last_cycle()->timing.clock_pulse == 28);
}

void test_fault_retry_keeps_same_beat() {
    using namespace colab::soc::core::mmu;
    Core core({{Opcode::Halt}});
    PageTableEntry page{0, false, {false, false, true, true}};
    core.mmu().enable_paging(0, [&](Address) { return page; });
    colab::simulation::Simulator<Core> simulator(core);
    simulator.step(); // IF/T1 已提交；IF/T2 的取指翻译将失败。
    for (int retry = 0; retry < 2; ++retry) {
        bool faulted = false;
        try { simulator.step(); }
        catch (const TranslationFault& fault) {
            faulted = true;
            assert(fault.reason() == FaultReason::NotPresent);
        }
        assert(faulted);
        assert(core.clock_pulses() == 1 && simulator.cycles() == 1);
        assert(core.completed_instructions() == 0);
        assert(core.last_cycle()->timing.instruction_beat == 1);
    }

    page.present = true;
    core.mmu().invalidate_page(0);
    core.mmu().clear_fault();
    simulator.step();
    const auto& timing = core.last_cycle()->timing;
    assert(timing.clock_pulse == 2 && timing.instruction_cycle == 1);
    assert(timing.stage == InstructionStage::Fetch);
    assert(timing.instruction_beat == 2 && timing.stage_beat == 2);
    simulator.run(6);
    assert(core.is_halted());
    assert(core.clock_pulses() == 8 && core.completed_instructions() == 1);
    assert(core.last_cycle()->instruction_complete);
}

int main() {
    test_instruction_cycles();
    test_fault_retry_keeps_same_beat();
}
