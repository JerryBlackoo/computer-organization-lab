#include "../soc/rv32/core.hpp"
#include <iostream>

using CPU = colab::soc::RV32Core;
namespace common = colab::devices::common;
static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
class DelayedBus : public common::DmaBus {
public:
    unsigned submissions = 0, writes = 0, reads = 0;
    std::uint32_t value = 0;
    common::Tag submit(common::Master, common::BusOperation operation, std::uint64_t address,
                       std::size_t bytes, common::Bytes data = {}) override {
        require(!pending && !response, "successor access bypassed outstanding predecessor");
        require(address == 32 && bytes == 4, "unexpected fenced transaction");
        if (operation == common::BusOperation::Read) require(writes == 1, "read observed an unacknowledged prior store");
        ++submissions; op = operation; payload = std::move(data); delay = 8; pending = true;
        return submissions;
    }
    void tick() {
        if (!pending || --delay) return;
        common::BusResult result{}; result.tag = submissions;
        result.status = colab::interconnect::pcie::Status::Success; result.transferred_bytes = 4;
        if (op == common::BusOperation::Write) { ++writes; value = common::load32(payload); }
        else { ++reads; result.data = common::word_bytes(value); }
        response = result; pending = false;
    }
    bool ready(common::Tag tag) const override { return response && response->tag == tag; }
    common::BusResult take(common::Tag tag) override {
        require(ready(tag), "fence consumed an unfinished response"); auto result = *response; response.reset(); return result;
    }
private:
    bool pending = false;
    unsigned delay = 0;
    common::BusOperation op{};
    common::Bytes payload;
    std::optional<common::BusResult> response;
};
static CPU make_cpu(std::vector<std::uint32_t> program, unsigned config) {
    return CPU(std::move(program), config == 1 ? CPU::Mode::Microprogrammed : CPU::Mode::Hardwired, 64, {},
        config >= 2 ? CPU::Execution::Pipelined : CPU::Execution::MultiCycle, {config != 3});
}
int main() {
    const std::uint32_t fences[]{0x0ff0000f, 0x8330000f, 0x0000000f, 0xffff828f, 0x0000100f, 0xffff928f};
    for (unsigned config = 0; config < 4; ++config) for (auto fence : fences) {
        auto cpu = make_cpu({0x02202023, fence, 0x02002183}, config);
        cpu.set_register(2, 42); cpu.set_register(5, 99); DelayedBus bus; cpu.connect_bus(bus);
        bool observed = false;
        for (unsigned n = 0; n < 1000 && cpu.completed_instructions() < 3; ++n) {
            bus.tick(); cpu.tick();
            if (cpu.completed_instructions() == 2 && cpu.last_cycle()->instruction_complete) {
                observed = true;
                require(bus.writes == 1 && bus.submissions == 1 && cpu.pc() == 8 && !cpu.last_retirement()->register_write &&
                    !cpu.last_retirement()->memory_write, "fence retired before predecessor or issued younger work");
                if (config >= 2) require(cpu.last_pipeline_cycle()->redirected && cpu.pipeline().empty(),
                    "fence did not clear prefetched pipeline state");
            }
        }
        require(observed && bus.reads == 1 && cpu.registers().read(3) == 42 && cpu.registers().read(5) == 99 &&
            cpu.bus_waits(), "fenced memory flow or ignored rd semantics differed");
        std::vector<std::uint32_t> user(64, 0x00000063); user[0] = 0x30200073; user[16] = fence;
        auto unprivileged = make_cpu(user, config);
        auto& state = unprivileged.interrupts(); state.write(0x3b0, 0x1fffffff); state.write(0x3a0, 0x1f);
        state.write(0x341, 64); state.write(0x300, 0);
        unprivileged.run_instructions(2, 1000);
        require(state.privilege() == colab::soc::core::PrivilegeMode::User && unprivileged.pc() == 68 && !state.exceptions(),
            "unprivileged fence trapped");
    }
    for (unsigned funct3 = 2; funct3 < 8; ++funct3) {
        bool rejected = false;
        try { (void)colab::soc::core::Decoder{}.decode_rv32((funct3 << 12) | 0xf); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "reserved MISC-MEM encoding accepted");
    }
    std::cout << "PASS FENCE/FENCE.I: delayed predecessors, no early successor effects, ignored fields, pipeline serialization and U-mode\n";
}
