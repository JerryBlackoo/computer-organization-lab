#include "../simulation/systemc/platform.hpp"
#include <iostream>
using namespace colab;
namespace sc = simulation::systemc;
static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int sc_main(int, char**) {
    try {
        // The default 256KiB board; all page tables and executable bytes are in its actual DIMM.
        sc::BootImage image{0x8000, {0x30200073}, {
            {0x1400, 0x801}, {0x2024, 0x244b}, // VA 0x40009000 -> PA 0x9000, S RXA
            {0x9000, 0x04900513}, {0x9004, 0x10500073}, {0x9008, 0x0000006f}}};
        sc::Platform machine("machine", image);
        auto poison_teaching = [&] {
            require(machine.cpu().translation_model() == soc::RV32Core::Translation::Architectural,
                "platform selected teaching translation semantics");
            machine.cpu().mmu().enable_paging(0, [](auto) -> soc::core::mmu::PageTableEntry {
                throw std::runtime_error("architectural fetch/data called host teaching MMU");
            });
        };
        auto prepare = [&] {
            poison_teaching();
            auto& s = machine.cpu().interrupts(); s.write(0x3b0, 0x1fffffff); s.write(0x3a0, 0x1f);
            s.write(0x180, 0x80000001); s.write(0x341, 0x40009000); s.write(0x300, 1u << 11);
        };
        auto until = [&](auto condition) {
            for (unsigned i = 0; i < 1000 && !condition(); ++i) machine.step();
            require(condition() && machine.fault().empty(), "Sv32 lifecycle timed out/faulted");
        };
        machine.power_on(); prepare();
        until([&] { return machine.cpu().sv32().busy() && machine.cpu().sv32().statistics().reads == 1; });
        machine.pause(); const auto pc = machine.cpu().pc(); const auto cycles = machine.cycles.read();
        // Pause gates all architectural phases, not the free-running clock signal.
        sc_core::sc_start(sc_core::sc_time(1, sc_core::SC_MS));
        require(machine.cpu().pc() == pc && machine.cycles.read() == cycles, "pause advanced an in-flight walk");
        machine.reset();
        require(machine.cpu().interrupts().read(0x180) == 0 && !machine.cpu().sv32().busy() &&
            machine.cpu().sv32().statistics().reads == 0 && machine.cpu().completed_instructions() == 0 &&
            machine.cpu().registers().read(10) == 0, "reset retained old walker/CPU state");
        prepare(); until([&] { return machine.cpu().waiting(); });
        require(machine.cpu().registers().read(10) == 73 && machine.cpu().completed_instructions() == 3 &&
            !machine.cpu().interrupts().exceptions(), "new epoch consumed stale response or failed translated fetch");
        // Repeat with power-off while a fresh walk is in flight; long clock time cannot revive it.
        machine.reset(); prepare(); until([&] { return machine.cpu().sv32().busy(); });
        machine.power_off(); sc_core::sc_start(sc_core::sc_time(10, sc_core::SC_MS));
        require(!machine.powered() && machine.cpu().clock_pulses() == 0 && !machine.cpu().sv32().busy(),
            "powered-off old page walk resumed");
        machine.power_on(); prepare(); until([&] { return machine.cpu().waiting(); });
        require(machine.cpu().registers().read(10) == 73 && machine.cpu().completed_instructions() == 3,
            "power cycle retained an abandoned page-table response");
        // Real Cache/TLM/DIMM and physical-map failures, not only a synthetic CPU bus.
        const auto unreachable_pte = static_cast<std::uint32_t>((SoCConfig{}.memory.capacity_bytes() >> 12) << 10);
        for (unsigned access=0; access<3; ++access) for (bool physical_error : {false,true})
        for (bool delegated : {false,true}) {
            machine.reset(); prepare();
            const bool fetch=access==0, store=access==2;
            const unsigned va=0x4000c000;
            machine.memory().write(0x9000,devices::common::word_bytes(store?0x00a0a023:0x0000a503));
            machine.memory().write(0x2030,devices::common::word_bytes(physical_error?(unreachable_pte|(fetch?0x4b:0xc7)):0));
            machine.cpu().set_register(1,va);machine.cpu().set_register(10,77);
            auto& s=machine.cpu().interrupts();
            const unsigned cause=physical_error?(fetch?1:store?7:5):(fetch?12:store?15:13);
            if(fetch) s.write(0x341,va);
            s.write(0x305,0x8801);s.write(0x105,0x40008801);s.write(0x302,delegated?1u<<cause:0);
            until([&]{return s.exceptions()==1;});
            const auto bank=delegated?0x140u:0x340u;
            require(s.read(bank+2)==cause&&s.read(bank+1)==(fetch?va:0x40009000)&&s.read(bank+3)==va&&
                machine.cpu().pc()==(delegated?0x40008800:0x8800)&&machine.cpu().completed_instructions()==1&&
                machine.cpu().registers().read(10)==77&&s.privilege()==(delegated?soc::core::PrivilegeMode::Supervisor:
                soc::core::PrivilegeMode::Machine),"real TLM fault/delegation lost precise architectural context");
        }
        std::cout << "PASS SystemC Sv32 lifecycle: default 256KiB, in-flight pause/reset/off, clean epoch, teaching-MMU isolation, 12 real TLM I/L/S page/access fault delegation cases\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
