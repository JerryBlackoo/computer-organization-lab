#include "../soc/core/wangdao09/execution.hpp"
#include <cassert>
#include <stdexcept>
using W = colab::soc::core::wangdao09::Execution;
int main() {
    for (auto bypass : {W::Bypass::Disabled, W::Bypass::Enabled}) {
        W cpu(bypass);
        cpu.set_register(1, 5); cpu.set_register(2, 100); cpu.set_register(3, 20);
        cpu.set_memory16(100, 6);
        cpu.begin(0x0240);
        cpu.tick();
        assert(cpu.last_cycle()->signals.y_in && cpu.reg(1) == 5);
        assert(cpu.registers().z.read() == (bypass == W::Bypass::Enabled ? 6 : 0));
        while (!cpu.done()) cpu.tick();
        assert(cpu.cycles() == (bypass == W::Bypass::Enabled ? 2u : 3u));
        assert(cpu.reg(1) == 6);
        cpu.begin(0x065e);
        cpu.tick(); assert(cpu.registers().mar.read() == 100 && cpu.last_cycle()->memory_address == 100);
        cpu.tick(); assert(cpu.registers().y.read() == 20 && cpu.registers().mdr.read() == 6);
        cpu.tick(); assert(cpu.registers().z.read() == 14 && cpu.reg(1) == 6);
        cpu.tick(); assert(cpu.done() && cpu.cycles() == 4 && cpu.reg(1) == 14);
        cpu.set_register(1, 0xffff); cpu.begin(0x0240);
        while (!cpu.done()) cpu.tick();
        assert(cpu.reg(1) == 0);
        bool rejected = false;
        try { cpu.begin(0x0241); } catch (const std::invalid_argument&) { rejected = true; }
        assert(rejected && cpu.done());
        cpu.set_register(2, 0xffff); cpu.begin(0x065e);
        rejected = false;
        try { cpu.tick(); } catch (const std::out_of_range&) { rejected = true; }
        assert(rejected && cpu.cycles() == 0 && cpu.reg(1) == 0);
    }
}
