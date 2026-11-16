#include "adapters.hpp"
#include "../../soc/controller/memory/memory.hpp"
#include "../../simulation/simulator.hpp"
#include <iostream>
int main(){
    namespace p = colab::interconnect::pcie;
    colab::dimm::DIMM dram({2,{4,{4,{2,4,4}}}});
    colab::soc::controller::Memory memory(dram,0x8000);
    colab::interconnect::PCIe bus;
    p::map_memory(bus,0x8000,64,memory,8,"slow DIMM window");
    p::map_memory(bus,0x8040,64,memory,1,"fast DIMM window");
    // Initialize through actual fabric writes, not an unrelated shadow array.
    const auto w=bus.submit({1,p::Operation::Write,0x8000,4,1,true,{1,2,3,4}});
    bus.run();if(bus.take(w).status!=p::Status::Success)return 1;
    const auto slow=bus.submit({0,p::Operation::Read,0x8000,4,1,true,{}});
    const auto fast=bus.submit({1,p::Operation::Read,0x8040,4,1,true,{}});
    colab::simulation::Simulator simulator(bus);
    while(!bus.idle()){
        simulator.step();const auto& t=bus.last_tick();
        constexpr const char* names[]={"Idle","Address","Wait","Ready","Response","Data","Release"};
        std::cout<<"event "<<t.tick<<" phase="<<names[static_cast<unsigned>(t.phase)];
        if(t.tag)std::cout<<" tag="<<*t.tag;
        std::cout<<" slot_busy="<<t.link_busy<<" complete="<<t.completed<<'\n';
        if(bus.ready(fast)){
            const auto r=bus.take(fast);
            std::cout<<"fast tag="<<r.tag<<" returns; slow_ready="<<bus.ready(slow)<<'\n';
        }
    }
    const auto r=bus.take(slow);
    std::cout<<"slow tag="<<r.tag<<" bytes="<<r.data.size()<<" first="<<unsigned(r.data.at(0))
             <<" DIMM[0]="<<dram.read(0)<<'\n';
}
