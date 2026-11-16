#include "../soc/core/rv32_core.hpp"
#include <iostream>
#include <random>

using CPU = colab::soc::RV32Core;
namespace common = colab::devices::common;
using colab::soc::core::RV32Fault;
using colab::soc::core::RV32FaultCause;
static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static std::uint32_t load(unsigned f, unsigned rd, unsigned base, int offset) {
    return ((std::uint32_t(offset) & 4095) << 20) | (base << 15) | (f << 12) | (rd << 7) | 3;
}
static std::uint32_t store(unsigned f, unsigned source, unsigned base, int offset) {
    const auto imm = std::uint32_t(offset) & 4095;
    return ((imm >> 5) << 25) | (source << 20) | (base << 15) | (f << 12) | ((imm & 31) << 7) | 0x23;
}
static CPU make_cpu(std::vector<std::uint32_t> program, unsigned config) {
    return CPU(std::move(program), config == 1 ? CPU::Mode::Microprogrammed : CPU::Mode::Hardwired,
        16, {}, config >= 2 ? CPU::Execution::Pipelined : CPU::Execution::MultiCycle, {config != 3});
}

// Byte-addressed reference backend: completion is delayed and requests keep their exact width.
class Bus : public common::DmaBus {
public:
    struct Request { common::BusOperation operation; std::uint64_t address; std::size_t count; common::Bytes data; };
    common::Bytes memory = common::Bytes(64);
    std::vector<Request> requests;
    bool malformed = false;
    common::Tag submit(common::Master, common::BusOperation op, std::uint64_t address,
                       std::size_t count, common::Bytes bytes = {}) override {
        require(!pending && !result, "duplicate outstanding data request");
        requests.push_back({op, address, count, std::move(bytes)});
        pending = true; clocks = 3;
        return requests.size();
    }
    void tick() {
        if (!pending || --clocks) return;
        const auto& request = requests.back();
        common::BusResult response{}; response.tag = requests.size();
        response.status = colab::interconnect::pcie::Status::Success;
        if (request.address > memory.size() || request.count > memory.size() - request.address)
            response.status = colab::interconnect::pcie::Status::Unmapped;
        else {
            response.transferred_bytes = request.count;
            if (request.operation == common::BusOperation::Read)
                response.data.assign(memory.begin() + request.address, memory.begin() + request.address + request.count);
            else {
                require(request.data.size() == request.count, "incorrect store payload width");
                std::copy(request.data.begin(), request.data.end(), memory.begin() + request.address);
            }
        }
        if (malformed) response.data.clear();
        result = response; pending = false;
    }
    bool ready(common::Tag tag) const override { return result && result->tag == tag; }
    common::BusResult take(common::Tag tag) override {
        require(ready(tag), "response taken before completion");
        auto response = *result; result.reset(); return response;
    }
private:
    bool pending = false;
    unsigned clocks = 0;
    std::optional<common::BusResult> result;
};
static void step(CPU& cpu, Bus* bus) {
    const auto before = cpu.completed_instructions();
    for (unsigned i = 0; i < 1000; ++i) {
        if (bus) bus->tick();
        cpu.tick();
        if (cpu.completed_instructions() != before) return;
    }
    throw std::runtime_error("subword instruction timed out");
}
static std::uint32_t reference(std::uint32_t value, unsigned bits, bool sign) {
    const auto modulus = std::int64_t{1} << bits;
    auto result = std::int64_t(value) % modulus;
    if (sign && result >= modulus / 2) result -= modulus;
    return static_cast<std::uint32_t>(result);
}
int main() {
    std::mt19937 random(4099);
    for (unsigned config = 0; config < 4; ++config) for (bool external : {false, true}) {
        for (unsigned sample = 0; sample < 40; ++sample) {
            const std::uint32_t edges[]{0, 0xffffffff, 0x80007fff, 0xff007f80, 0x00018000};
            const auto word = sample < 5 ? edges[sample] : random();
            for (unsigned f : {0u, 1u, 4u, 5u}) {
                const unsigned bytes = (f & 1) ? 2 : 1;
                for (unsigned offset = 0; offset < 4; offset += bytes) {
                    auto cpu = make_cpu({load(f, 3, 1, int(offset) - 4), 0x00118213}, config);
                    Bus bus;
                    cpu.set_register(1, 20); cpu.set_memory(16, word);
                    for (unsigned i = 0; i < 4; ++i) bus.memory[16 + i] = word >> (8 * i);
                    if (external) cpu.connect_bus(bus);
                    step(cpu, external ? &bus : nullptr);
                    const auto expected = reference(word >> (offset * 8), bytes * 8, f < 4);
                    require(cpu.registers().read(3) == expected, "load extension or byte lane incorrect");
                    step(cpu, external ? &bus : nullptr);
                    require(cpu.registers().read(4) == expected + 1, "load-use forwarding/interlock failed");
                    if (external) require(bus.requests.size() == 1 && bus.requests[0].count == bytes &&
                        bus.requests[0].address == 16 + offset && cpu.bus_waits(), "load transaction was widened or repeated");
                }
            }
        }
        auto cpu = make_cpu({load(0, 3, 1, 0), store(0, 3, 1, 1), load(5, 4, 1, 2),
            store(1, 4, 1, 4), store(0, 12, 1, 0), store(1, 12, 1, 2), load(0, 0, 1, 3)}, config);
        Bus bus;
        cpu.set_register(1, 16); cpu.set_register(12, 0x12345678);
        cpu.set_memory(16, 0x80807fff); cpu.set_memory(20, 0xaabbccdd);
        bus.memory[16] = 255; bus.memory[17] = 127; bus.memory[18] = 128; bus.memory[19] = 128;
        bus.memory[20] = 0xdd; bus.memory[21] = 0xcc; bus.memory[22] = 0xbb; bus.memory[23] = 0xaa;
        if (external) cpu.connect_bus(bus);
        for (unsigned i = 0; i < 7; ++i) {
            step(cpu, external ? &bus : nullptr);
            if (i == 1 || i == 3 || i == 4 || i == 5) {
                const auto& write = cpu.last_retirement()->memory_write;
                require(write && write->bytes == (i == 3 || i == 5 ? 2u : 1u), "retirement lost store width");
                require(write->value == (i == 1 ? 255u : i == 3 ? 0x8080u : i == 4 ? 0x78u : 0x5678u),
                    "retirement did not truncate store data");
            }
        }
        require(cpu.registers().read(0) == 0, "subword load changed x0");
        if (external) {
            require(bus.memory[16] == 0x78 && bus.memory[17] == 0xff && bus.memory[18] == 0x78 &&
                bus.memory[19] == 0x56 && bus.memory[20] == 0x80 && bus.memory[21] == 0x80 &&
                bus.memory[22] == 0xbb && bus.memory[23] == 0xaa, "external store clobbered adjacent bytes");
            require(bus.requests.size() == 7 && bus.requests.back().count == 1, "x0 load lost its bus effect");
        } else require(cpu.memory().read(16) == 0x5678ff78 && cpu.memory().read(20) == 0xaabb8080,
            "local store clobbered adjacent bytes");

        for (bool store_fault : {false, true}) for (bool misaligned : {false, true}) {
            auto fault_cpu = make_cpu({store_fault ? store(1, 2, 1, 0) : load(1, 3, 1, 0)}, config);
            Bus fault_bus;
            const auto address = misaligned ? 17u : 64u;
            fault_cpu.set_register(1, address); fault_cpu.set_register(2, 0x1234); fault_cpu.set_register(3, 99);
            if (external) fault_cpu.connect_bus(fault_bus);
            bool caught = false;
            try { step(fault_cpu, external ? &fault_bus : nullptr); }
            catch (const RV32Fault& fault) {
                const auto expected = misaligned
                    ? (store_fault ? RV32FaultCause::StoreAddressMisaligned : RV32FaultCause::LoadAddressMisaligned)
                    : (store_fault ? RV32FaultCause::StoreAccessFault : RV32FaultCause::LoadAccessFault);
                caught = fault.cause() == expected && fault.pc() == 0 && fault.value() == address;
            }
            require(caught && fault_cpu.pc() == 0 && !fault_cpu.completed_instructions() &&
                fault_cpu.registers().read(3) == 99, "halfword fault was imprecise");
            if (misaligned) require(fault_bus.requests.empty(), "misaligned request reached bus");
        }
        // Last naturally aligned scalar in RAM is legal; no hidden word-width range check.
        auto edge_cpu = make_cpu({load(4, 3, 1, 3), load(5, 4, 1, 2), store(0, 2, 1, 3), store(1, 2, 1, 2)}, config);
        Bus edge_bus;
        edge_cpu.set_register(1, 60); edge_cpu.set_register(2, 0x12345678); edge_cpu.set_memory(60, 0x80800000);
        edge_bus.memory[62] = 0x80; edge_bus.memory[63] = 0x80;
        if (external) edge_cpu.connect_bus(edge_bus);
        step(edge_cpu, external ? &edge_bus : nullptr); step(edge_cpu, external ? &edge_bus : nullptr);
        require(edge_cpu.registers().read(3) == 0x80 && edge_cpu.registers().read(4) == 0x8080,
            "last byte/halfword in RAM was not readable");
        step(edge_cpu, external ? &edge_bus : nullptr); step(edge_cpu, external ? &edge_bus : nullptr);
        require(external ? edge_bus.memory[62] == 0x78 && edge_bus.memory[63] == 0x56 :
            edge_cpu.memory().read(60) == 0x56780000, "last byte/halfword in RAM was not writable");

        auto malformed_cpu = make_cpu({load(4, 3, 1, 0)}, config);
        Bus malformed_bus; malformed_bus.malformed = true;
        malformed_cpu.set_register(1, 16); malformed_cpu.connect_bus(malformed_bus);
        bool caught = false;
        try { step(malformed_cpu, &malformed_bus); }
        catch (const RV32Fault& fault) { caught = fault.cause() == RV32FaultCause::LoadAccessFault; }
        require(caught && !malformed_cpu.completed_instructions(), "short load response escaped validation");
    }
    for (unsigned f : {3u, 6u, 7u}) {
        bool rejected = false;
        try { (void)colab::soc::core::Decoder{}.decode_rv32(load(f, 1, 0, 0)); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "reserved load accepted");
    }
    std::cout << "PASS RV32 byte/halfword loads/stores: four CPU configurations, delayed bus, sign extension, hazards and precise faults\n";
}
