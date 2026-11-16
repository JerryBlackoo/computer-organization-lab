#include "../interconnect/pcie/adapters.hpp"
#include "../soc/controller/memory/memory.hpp"
#include "../simulation/memory_access.hpp"
#include "../simulation/simulator.hpp"
#include <cassert>
#include <iostream>
using namespace colab::interconnect::pcie;

template<class E,class F> void throws(F f){bool caught=false;try{f();}catch(const E&){caught=true;}assert(caught);}
Request read(Address a,std::size_t beats=1,bool burst=true,Master master=0){
    return {master,Operation::Read,a,4,beats,burst,{}};
}
Request write(Address a,Bytes bytes,bool burst=true,Master master=0){
    const auto beats=bytes.size()/4;
    return {master,Operation::Write,a,4,beats,burst,std::move(bytes)};
}
struct System {
    colab::dimm::DIMM dimm{{2,{4,{4,{2,4,4}}}}};
    colab::soc::controller::Memory memory{dimm,0x8000};
};

void memory_end_to_end(){
    System s;
    Fabric bus;
    map_memory(bus,0x8000,128,s.memory,2,"real DIMM");
    const auto w=bus.submit(write(0x8000,{1,2,3,4,5,6,7,8}));
    assert(!bus.ready(w) && s.dimm.read(0)==0);
    bus.run();
    auto result=bus.take(w);
    assert(result.status==Status::Success && !result.read_completion && result.transferred_bytes==8);
    assert(s.dimm.read(0)==0x0201 && s.dimm.read(3)==0x0807);
    const auto r=bus.submit(read(0x8000,2));bus.run();
    result=bus.take(r);
    assert(result.data==Bytes({1,2,3,4,5,6,7,8}) && result.read_completion);
    assert(bus.statistics().posted_writes==1 && bus.statistics().read_completions==1);
    assert(s.memory.statistics().write_requests==2 && s.memory.statistics().read_requests==2);
    // The same adapter can use the existing virtual-to-physical MemoryAccess wiring.
    colab::soc::core::MMU mmu;
    colab::simulation::MemoryAccess port(mmu,s.memory.reader(),s.memory.writer());
    Fabric translated;
    map_memory(translated,0x8000,128,port);
    const auto tag=translated.submit(read(0x8004));translated.run();
    assert(translated.take(tag).data==Bytes({5,6,7,8}));
    std::cout<<"end-to-end: fabric -> Memory / MemoryAccess -> controller -> DIMM passed\n";
}

void burst_addresses(){
    for(auto timing:{Timing::Synchronous,Timing::Asynchronous,Timing::SemiSynchronous,Timing::Split}){
        System s;
        std::uint64_t burst_ticks=0;
        for(bool burst:{true,false}){
            Fabric bus({timing,1,64});
            std::vector<Address> addresses;
            bus.map({0x8000,128,1,[&](Address a,std::size_t n){addresses.push_back(a);return s.memory.read(a,n);},
                s.memory.writer(),"DIMM"});
            const auto tag=bus.submit(read(0x8000,4,burst));
            bus.run();assert(bus.take(tag).status==Status::Success);
            assert(addresses==std::vector<Address>({0x8000,0x8004,0x8008,0x800C}));
            assert(bus.statistics().address_phases==(burst?1:4));
            if(burst)burst_ticks=bus.statistics().ticks;
            else assert(bus.statistics().ticks>burst_ticks);
        }
    }
    std::cout<<"4-beat burst: addresses +4 bytes, 1 address phase vs non-burst 4 (all timings)\n";
}

void timing_and_handshake(){
    System s;
    Fabric synchronous({Timing::Synchronous,2,8});map_memory(synchronous,0x8000,128,s.memory,5);
    auto tag=synchronous.submit(read(0x8000));synchronous.run();
    assert(synchronous.statistics().ticks==3 && synchronous.take(tag).status==Status::Timeout);
    assert(s.memory.statistics().read_requests==0 && synchronous.idle());
    Fabric fixed({Timing::Synchronous,4,8});map_memory(fixed,0x8000,128,s.memory,1);
    tag=fixed.submit(read(0x8000));fixed.run();assert(fixed.statistics().ticks==6);
    assert(fixed.take(tag).status==Status::Success);
    Fabric semi({Timing::SemiSynchronous,1,8});map_memory(semi,0x8000,128,s.memory,5);
    tag=semi.submit(read(0x8000));unsigned waits=0;
    while(!semi.idle()){
        semi.tick();const auto t=semi.last_tick();
        if(!t.wait_n){++waits;assert(t.phase==Phase::Wait && t.link_busy);}
    }
    assert(waits==4 && semi.statistics().ticks==7 && semi.take(tag).status==Status::Success);
    Fabric async({Timing::Asynchronous,1,8});map_memory(async,0x8000,128,s.memory,5);
    tag=async.submit(read(0x8000));
    std::vector<std::pair<bool,bool>> changes;
    changes.push_back({false,false});
    while(!async.idle()){
        async.tick();const auto& t=async.last_tick();
        const auto pair=std::make_pair(t.req,t.ack);
        if(pair!=changes.back())changes.push_back(pair);
    }
    assert((changes==std::vector<std::pair<bool,bool>>{{false,false},{true,false},{true,true},{false,true},{false,false}}));
    assert(async.take(tag).status==Status::Success);
    std::cout<<"sync slow device: bounded timeout; semi-sync WAIT low=4 ticks; async four-phase REQ/ACK ordered\n";
}

void split_and_arbitration(){
    System s;
    Fabric bus;
    map_memory(bus,0x8000,64,s.memory,12,"slow");
    map_memory(bus,0x8040,64,s.memory,1,"fast");
    s.memory.write(0x8000,{1,2,3,4});
    s.memory.write(0x8040,{5,6,7,8});
    const auto slow=bus.submit(read(0x8000,1,true,1));
    const auto fast=bus.submit(read(0x8040,1,true,2));
    assert(!bus.ready(slow)&&!bus.ready(fast));
    bus.tick();assert(bus.last_tick().tag==slow && bus.last_tick().phase==Phase::Address);
    bus.tick();assert(bus.last_tick().tag==fast && bus.last_tick().phase==Phase::Address);
    while(!bus.ready(fast))bus.tick();
    assert(!bus.ready(slow));
    const auto fast_result=bus.take(fast);
    assert(fast_result.master==2 && fast_result.data==Bytes({5,6,7,8}));
    bus.run();const auto slow_result=bus.take(slow);
    assert(slow_result.master==1 && slow_result.data==Bytes({1,2,3,4}) && bus.idle());
    assert(bus.statistics().idle_ticks>0);
    // Admission fairness: master 1 cannot monopolize the queued master 2.
    Fabric fair;
    map_memory(fair,0x8000,128,s.memory,10);
    const auto a=fair.submit(read(0x8000,1,true,1));
    const auto b=fair.submit(read(0x8004,1,true,1));
    const auto c=fair.submit(read(0x8008,1,true,2));
    fair.tick();assert(fair.last_tick().tag==a);
    fair.tick();assert(fair.last_tick().tag==c);
    fair.tick();assert(fair.last_tick().tag==b);
    fair.run();
    std::cout<<"split: slow request releases link; later fast tag returns first; round-robin master grants\n";
}

void validation_and_error_cleanup(){
    throws<std::invalid_argument>([]{Fabric bad({Timing::Split,0,4});});
    throws<std::invalid_argument>([]{Fabric bad({Timing::Split,1,0});});
    throws<std::invalid_argument>([]{Fabric bad({static_cast<Timing>(99),1,4});});
    System s;
    for(auto timing:{Timing::Synchronous,Timing::Asynchronous,Timing::SemiSynchronous,Timing::Split}){
        Fabric bus({timing,1,8});map_memory(bus,0x8000,128,s.memory);
        throws<std::invalid_argument>([&]{map_memory(bus,0x8070,16,s.memory);});
        throws<std::invalid_argument>([&]{map_memory(bus,0,0,s.memory);});
        throws<std::invalid_argument>([&]{map_memory(bus,0xFFFFFFF0,32,s.memory);});
        throws<std::invalid_argument>([&]{bus.submit(read(0x8001));});
        throws<std::invalid_argument>([&]{auto r=read(0x8000);r.beat_bytes=3;bus.submit(r);});
        throws<std::invalid_argument>([&]{bus.submit(read(0x8000,0));});
        throws<std::out_of_range>([&]{bus.submit(read(0xFFFFFFFC,2));});
        throws<std::invalid_argument>([&]{bus.submit({0,Operation::Write,0x8000,4,1,true,{1}});});
        auto tag=bus.submit(read(0x9000));bus.run();assert(bus.take(tag).status==Status::Unmapped);
        tag=bus.submit(read(0x807C,2));bus.run();assert(bus.take(tag).status==Status::Unmapped);
        // Verify a failure cannot wedge the next queued transaction.
        bus.map({0x9000,16,1,[](Address,std::size_t)->Bytes{throw std::runtime_error("device fault");},{},"failing"});
        const auto failed=bus.submit(read(0x9000));
        const auto good=bus.submit(read(0x8000));
        throws<std::logic_error>([&]{map_memory(bus,0xA000,16,s.memory);});
        bus.run();assert(bus.take(failed).status==Status::BackendError);
        assert(bus.take(good).status==Status::Success && bus.idle());
        tag=bus.submit(write(0x9000,{1,2,3,4}));bus.run();assert(bus.take(tag).status==Status::BackendError);
        bus.map({0xA000,16,1,[](Address,std::size_t){return Bytes{0};},{},"short read"});
        tag=bus.submit(read(0xA000));bus.run();assert(bus.take(tag).status==Status::BackendError);
        assert(bus.outstanding()==0 && bus.idle());
    }
    Fabric bounded({Timing::Split,1,1});map_memory(bounded,0x8000,128,s.memory);
    const auto tag=bounded.submit(read(0x8000));
    throws<std::length_error>([&]{bounded.submit(read(0x8004));});
    throws<std::out_of_range>([&]{bounded.take(tag);});
    throws<std::runtime_error>([&]{bounded.run(1);});
    bounded.run();assert(bounded.take(tag).status==Status::Success);
    throws<std::out_of_range>([&]{bounded.take(tag);});
}
void widths_partial_failure_and_permissions(){
    for(const auto timing:{Timing::Synchronous,Timing::Asynchronous,Timing::SemiSynchronous,Timing::Split}){
        System s;
        Fabric bus({timing,1,64});map_memory(bus,0x8000,128,s.memory);
        for(const std::size_t width:{1u,2u,4u,8u}){
            Bytes payload(width*3);
            for(std::size_t i=0;i<payload.size();++i)payload[i]=static_cast<std::uint8_t>(i+width);
            auto tag=bus.submit({0,Operation::Write,0x8000,width,3,true,payload});bus.run();
            assert(bus.take(tag).status==Status::Success);
            tag=bus.submit({0,Operation::Read,0x8000,width,3,false,{}});bus.run();
            assert(bus.take(tag).data==payload);
        }
        unsigned writes=0;
        bus.map({0x9000,16,1,{},[&](Address a,const Bytes& b){
            if(a==0x9004)throw std::runtime_error("second beat fails");
            ++writes;s.memory.write(0x8000,b);
        },"partial failure"});
        const auto tag=bus.submit(write(0x9000,{10,20,30,40,50,60,70,80}));bus.run();
        const auto r=bus.take(tag);
        assert(r.status==Status::BackendError && r.transferred_bytes==4 && writes==1);
        assert(s.memory.read(0x8000,4)==Bytes({10,20,30,40}) && bus.idle());
    }
    using namespace colab::soc::core::mmu;
    colab::soc::core::MMU mmu;
    PageTableEntry page{0,true,{true,false,false,true}};
    mmu.enable_paging(0,[&](Address){return page;});
    unsigned physical_writes=0;
    colab::simulation::MemoryAccess access(mmu,
        [](Address,std::size_t n){return Bytes(n);},
        [&](Address,const Bytes&){++physical_writes;});
    Fabric bus;map_memory(bus,0,128,access);
    auto tag=bus.submit(write(0,{1,2,3,4}));bus.run();
    assert(bus.take(tag).status==Status::BackendError && physical_writes==0);
    tag=bus.submit(read(0));bus.run();assert(bus.take(tag).status==Status::Success);
    std::cout<<"widths 1/2/4/8, partial burst error accounting, MMU write protection passed\n";
}

int main(){memory_end_to_end();burst_addresses();timing_and_handshake();split_and_arbitration();validation_and_error_cleanup();widths_partial_failure_and_permissions();}
