#include "../simulation/systemc/platform.hpp"
#include <iostream>
using namespace colab;
namespace sc = simulation::systemc;
namespace common = devices::common;
using N = devices::nvme::Controller;
using H = sc::Platform::HddController;
using C = devices::NIC;
static void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
static common::Bytes pattern(unsigned seed) {
    common::Bytes b(512); for (unsigned i=0;i<b.size();++i) b[i]=static_cast<unsigned char>(i*73+seed); return b;
}
static common::Bytes command(unsigned opcode,unsigned cid,unsigned prp,unsigned nsid,unsigned cdw10,unsigned cdw11) {
    common::Bytes b(64); common::store32(b,0,opcode|(cid<<16)); common::store32(b,4,nsid);
    common::store32(b,24,prp); common::store32(b,40,cdw10); common::store32(b,44,cdw11); return b;
}
struct Fixture {
    sc::Platform& m; std::string device;
    template<class Predicate> void until(Predicate predicate,const char* message) {
        for(unsigned i=0;i<100000 && !predicate();++i) { m.step(); require(m.fault().empty(),"CPU fault during reset fixture"); }
        require(predicate(),message);
    }
    void put(unsigned address,unsigned value) {
        const auto tag=m.debug_port().submit(0,common::BusOperation::Write,address,4,common::word_bytes(value));
        until([&]{return m.debug_port().ready(tag);},"MMIO timeout");
        require(m.debug_port().take(tag).status==interconnect::pcie::Status::Success,"MMIO error");
    }
    void configure() {
        m.memory().write(0x4ff0,common::Bytes(544,0xa5));
        m.memory().write(0x5000,common::Bytes(512,0));
        if(device=="nvme") {
            put(SoC::NvmeBase+N::AdminAttributes,0x30003);put(SoC::NvmeBase+N::AdminSq,0x1000);
            put(SoC::NvmeBase+N::AdminCq,0x2000);put(SoC::NvmeBase+N::Config,0x460001);
            until([&]{return m.nvme().state()==N::State::Idle;},"NVMe enable timeout");
            m.memory().write(0x1000,command(5,1,0x4000,0,0x30001,3));
            m.memory().write(0x1040,command(1,2,0x3000,0,0x30001,0x10001));
            put(SoC::NvmeBase+N::Doorbell,2);
            until([&]{return m.nvme().completed_commands()==2;},"NVMe admin timeout");
            put(SoC::NvmeBase+N::Doorbell+4,2);
        } else if(device=="hdd") {
            until([&]{return m.disk().ready();},"HDD spin-up timeout");
            put(SoC::HddBase+H::Length,512);put(SoC::HddBase+H::DmaAddress,0x5000);
            put(SoC::HddBase+H::Control,H::EnableDma|H::EnableIrq);
        } else {
            put(SoC::NicBase+C::TxAddress,0x7000);put(SoC::NicBase+C::RxAddress,0x5000);put(SoC::NicBase+C::Length,512);
        }
        require(!m.irq_pending.read(),"setup left an IRQ pending");
    }
    void start(unsigned lba,bool write=false) {
        if(device=="nvme") {
            if(write) m.memory().write(0x5000,pattern(47));
            m.memory().write(0x3000,command(write?1:2,10,0x5000,1,lba,0));
            put(SoC::NvmeBase+N::Doorbell+8,1);
        } else if(device=="hdd") { put(SoC::HddBase+H::Lba,lba);put(SoC::HddBase+H::Command,H::Read); }
        else { m.memory().write(0x7000,pattern(lba?47:5));put(SoC::NicBase+C::Command,1); }
    }
    bool completed() {
        return device=="nvme" ? m.nvme().completed_commands()==3 : device=="hdd" ?
            m.hdd().state()==devices::hdd::DeviceState::Completed : m.nic().mmio_read32(C::Status)==2;
    }
    void wait_boundary(unsigned stage) {
        if(stage==0) until([&]{return device=="nvme" ? m.nvme().state()==N::State::Fetching : device=="hdd" ?
            m.disk().busy() : m.nic().mmio_read32(C::Status)==1 && m.bus().fabric().last_tick().master==SoC::NicMaster;
        },"no early in-flight state");
        else if(stage==1) until([&]{
            const auto prefix=m.memory().read(0x5000,32),suffix=m.memory().read(0x51e0,32);
            const auto expected=pattern(5);
            return prefix==common::Bytes(expected.begin(),expected.begin()+32) && suffix==common::Bytes(32,0);
        },"no partially committed DMA state");
        else if(stage==2) until([&]{return completed() && m.irq_pending.read();},"no posted completion/IRQ");
        else until([&]{return m.nvme().state()==N::State::MediaWait;},"no NVMe media-write wait");
    }
    void check_idle() {
        require(!m.irq_pending.read() && m.cpu().interrupts().entries()==0,"old IRQ crossed reset/power epoch");
        require(m.nvme().state()==N::State::Disabled && m.nvme().completed_commands()==0 &&
            m.hdd().state()==devices::hdd::DeviceState::Idle && m.hdd().mmio_read32(H::IrqStatus)==0 &&
            m.nic().mmio_read32(C::Status)==0 && m.nic().mmio_read32(C::Irq)==0,
            "old device completion/state survived reset");
    }
    void acknowledge() {
        if(device=="nvme") put(SoC::NvmeBase+N::Doorbell+12,1);
        else if(device=="hdd") put(SoC::HddBase+H::IrqStatus,1);
        else put(SoC::NicBase+C::Irq,1);
        require(!m.irq_pending.read(),"fresh completion IRQ failed to deassert");
    }
};
int sc_main(int argc,char** argv) {
    try {
        const std::string device=argc>1?argv[1]:"nvme";
        require(device=="nvme"||device=="hdd"||device=="nic","unknown device");
        SoCConfig config;config.disk.spin_up_ms=1;
        sc::Platform machine("computer",{0x8000,{0x0000006f},{}},sc_core::sc_time(50,sc_core::SC_US),config);
        machine.nand().write(0,pattern(5));machine.nand().write(1,pattern(47));
        machine.disk().power_on();machine.disk().advance(1);
        for(unsigned lba=0;lba<2;++lba) {
            machine.disk().submit_write_lba(lba,pattern(lba?47:5));
            require(machine.disk().run_until_idle(100),"HDD fixture write timeout");
            const auto result=machine.disk().take_completion();require(result&&result->status==devices::hdd::Status::Success,"HDD fixture write failed");
        }
        machine.power_on();Fixture f{machine,device};unsigned cuts=0;
        for(unsigned stage=0;stage<(device=="nvme"?4u:3u);++stage) for(bool off:{false,true}) {
            machine.reset();f.configure();f.start(0,stage==3);f.wait_boundary(stage);
            const auto nand=machine.nand().read(0,2);
            if(stage==3) require(machine.nand().read(0,1)==pattern(5),"write-wait boundary was already committed");
            const auto before_cut=machine.memory().read(0x1000,0x7000);
            if(off) {
                machine.power_off();const auto retained=machine.memory().read(0x1000,0x7000);
                require(retained==before_cut,"power-off rolled back committed RAM writes");
                sc_core::sc_start(sc_core::sc_time(20,sc_core::SC_MS));
                require(machine.memory().read(0x1000,0x7000)==retained,"powered-off DMA changed RAM");
                machine.power_on();
            } else {
                machine.reset();
                require(machine.memory().read(0x1000,0x7000)==before_cut,"warm reset discarded committed RAM writes");
            }
            f.check_idle();const auto retained=machine.memory().read(0x1000,0x7000);
            machine.run(2000);f.check_idle();
            require(machine.memory().read(0x1000,0x7000)==retained,"old DMA/CQE wrote after reset");
            require(machine.nand().read(0,2)==nand,"cancelled NVMe write committed after reset");
            f.configure();f.start(1);
            f.until([&]{return f.completed()&&machine.irq_pending.read();},"fresh request stalled after reset");
            require(machine.memory().read(0x5000,512)==pattern(47),"fresh descriptor consumed stale response/data");
            require(machine.memory().read(0x4ff0,16)==common::Bytes(16,0xa5)&&machine.memory().read(0x5200,16)==common::Bytes(16,0xa5),
                "DMA overwrote adjacent bytes");
            f.acknowledge();++cuts;
        }
        std::cout<<"PASS "<<device<<" reset windows: "<<cuts<<" cuts (early, partial DMA, posted IRQ"<<(device=="nvme"?", pending media write":"")
            <<" x reset/off), no late RAM/CQE/IRQ, same-buffer new descriptor + IRQ recovery\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
