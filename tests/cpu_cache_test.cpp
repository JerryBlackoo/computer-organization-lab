#include "io_test_support.hpp"
#include "rv32_test_support.hpp"
#include "../soc/shared/mmu/page_table_port/page_table_port.hpp"
#include <random>

using namespace io_test;
using Port = soc::cache::CpuPort;
using Status = interconnect::pcie::Status;
namespace r = rv32_test;

static void retire(SoC& system) {
    const auto before = system.cpu().completed_instructions();
    until(system, [&] { return system.cpu().completed_instructions() > before; }, 10000);
}

static void cpu_hit_miss_and_stores(SoCConfig cpu_config) {
    SoC system({r::i(3, 1, 0, true), r::i(4, 1, 0, true), r::i(5, 1, 4, true),
        r::sw(2, 1, 0), r::i(6, 1, 0, true), r::sw(2, 1, 64), r::i(7, 1, 64, true)}, cpu_config);
    system.cpu().set_register(1, 0x1000); system.cpu().set_register(2, 0xAABBCCDD);
    system.memory().write(0x1000, c::word_bytes(0x12345678));
    system.memory().write(0x1004, c::word_bytes(0x87654321));
    retire(system);
    assert(system.cpu().registers().read(3) == 0x12345678);
    assert(system.data_cache().statistics().read_misses == 1 && system.data_cache().statistics().fills == 1);
    assert(system.memory().statistics().bytes_read == system.data_cache().config().line_bytes);
    const auto reads = system.memory().statistics().read_requests;
    const auto completed = system.bus().fabric().statistics().completed;
    const auto waits = system.cpu().bus_waits();
    assert(waits > 0);
    retire(system); retire(system);
    assert(system.cpu().registers().read(4) == 0x12345678 && system.cpu().registers().read(5) == 0x87654321);
    assert(system.data_cache().statistics().read_hits == 2);
    assert(system.memory().statistics().read_requests == reads);
    assert(system.bus().fabric().statistics().completed == completed);
    if (system.cpu().execution() == soc::RV32Core::Execution::Pipelined) {
        // The two hit loads add no waits. In the third load's WB cycle the
        // younger store has already issued from MEM and contributes one wait.
        assert(system.cpu().bus_waits() == waits + 1);
        assert(system.cpu().last_pipeline_cycle()->memory_stall && system.cpu().pipeline().ex_mem().pc == 12);
    } else assert(system.cpu().bus_waits() == waits);
    retire(system);
    assert(c::load32(system.memory().read(0x1000, 4)) == 0xAABBCCDD); // SW 等写穿确认后才退休。
    assert(system.data_cache().statistics().write_hits == 1);
    retire(system);
    assert(system.cpu().registers().read(6) == 0xAABBCCDD);
    const auto fills = system.data_cache().statistics().fills;
    retire(system);
    assert(system.data_cache().statistics().fills == fills); // 写缺失不分配行。
    assert(system.data_cache().statistics().write_misses == 1);
    retire(system);
    assert(system.cpu().registers().read(7) == 0xAABBCCDD);
    assert(system.data_cache().statistics().fills == 2 && system.data_cache().statistics().writebacks == 0);
    // 宿主经 Memory 初始化/修复同样通知缓存，不保留陈旧副本。
    system.memory().write(0x1040, c::word_bytes(42));
    const auto& decoder = system.data_cache().address_decoder();
    const auto parts = decoder.decode(0x1040);
    bool updated = false;
    for (std::size_t way = 0; way < system.data_cache().config().ways; ++way) {
        const auto& line = system.data_cache().line(parts.set_index, way);
        if (line.valid && line.tag == parts.tag) { updated = true; assert(c::load32(line.data) == 42); }
    }
    assert(updated);
}

static void mmu_before_cache(SoCConfig cpu_config) {
    using namespace soc::core::mmu;
    SoC system({r::i(3, 1, 0, true), r::i(4, 2, 0, true), r::sw(3, 1, 0)}, cpu_config);
    system.cpu().set_register(1, 0x1000); system.cpu().set_register(2, 0x3000);
    system.memory().write(0x2000, c::word_bytes(0x11223344));
    PageTablePort table(system.memory().reader(), system.memory().writer());
    PageTableEntry entry{2, true, {true, false, false, true}, true, false, 0};
    table.writer()(0x4008, entry); table.writer()(0x4018, entry);
    system.cpu().mmu().enable_paging(0x4000, table.reader(), table.writer());
    retire(system); retire(system);
    assert(system.cpu().registers().read(3) == 0x11223344 && system.cpu().registers().read(4) == 0x11223344);
    assert(system.data_cache().statistics().fills == 1 && system.data_cache().statistics().read_hits == 1);
    const auto transactions = system.bus().fabric().statistics().completed;
    const auto writes = system.memory().statistics().write_requests;
    for (unsigned retry = 0; retry < 2; ++retry) {
        bool denied = false;
        try { retire(system); }
        catch (const TranslationFault& fault) { denied = fault.reason() == FaultReason::WriteDenied; }
        assert(denied && system.cpu().completed_instructions() == 2 && system.cpu().pc() == 8);
    }
    assert(system.data_cache().statistics().write_hits == 0 && system.data_cache().statistics().write_misses == 0);
    assert(system.bus().fabric().statistics().completed == transactions && system.memory().statistics().write_requests == writes);
    // 缺页也不能凭物理 Cache 已有副本绕过检查。
    entry.present = false; table.writer()(0x4008, entry); system.cpu().mmu().invalidate_page(1);
    bool missing = false;
    try { retire(system); }
    catch (const TranslationFault& fault) { missing = fault.reason() == FaultReason::NotPresent; }
    assert(missing && system.bus().fabric().statistics().completed == transactions);
}

static void mmio_is_uncached(SoCConfig cpu_config) {
    SoC system({r::i(3, 1, 0, true), r::i(4, 1, 0, true)}, cpu_config);
    system.cpu().set_register(1, SoC::IrqBase);
    retire(system);
    assert(system.cpu().registers().read(3) == 0);
    system.interrupts().set_irq(1, true);
    retire(system);
    assert(system.cpu().registers().read(4) == 2);
    assert(system.data_port().statistics().bypasses == 2);
    assert(system.data_cache().statistics().hits() == 0 && system.data_cache().statistics().misses() == 0);
}

static void pmp_before_cache(SoCConfig cpu_config) {
    std::vector<std::uint32_t> program(64, r::beq(0, 0, 0));
    program[0] = r::i(3, 1, 0, true); program[1] = 0x30200073; // Warm a machine-only data line, then drop to U.
    program[16] = r::i(4, 1, 0, true);
    SoC system(program, cpu_config);
    system.cpu().set_register(1, 0x1000); system.memory().write(0x1000, c::word_bytes(0xcafebabe));
    auto& state = system.cpu().interrupts();
    state.write(0x3b0, 64 / 4); state.write(0x3b1, 128 / 4); state.write(0x3a0, 0x0d00);
    state.write(0x341, 64); state.write(0x300, 0);
    retire(system); retire(system);
    assert(state.privilege() == soc::core::PrivilegeMode::User && system.data_cache().contains(0x1000, 4));
    const auto transactions = system.bus().fabric().statistics().completed;
    const auto reads = system.memory().statistics().read_requests;
    const auto hits = system.data_cache().statistics().read_hits;
    bool denied = false;
    try { retire(system); }
    catch (const soc::core::RV32Fault& fault) {
        denied = fault.cause() == soc::core::RV32FaultCause::LoadAccessFault && fault.pc() == 64 && fault.value() == 0x1000;
    }
    assert(denied && system.cpu().completed_instructions() == 2 && system.cpu().registers().read(4) == 0);
    assert(system.bus().fabric().statistics().completed == transactions && system.memory().statistics().read_requests == reads &&
        system.data_cache().statistics().read_hits == hits);
}

static void dma_coherence(SoCConfig cpu_config) {
    // 缓存旧数据和 ready=0 后，由 Guest 反复 LW 等待 DMA 写入的 ready=1。
    SoC system({r::i(3, 1, 0, true), r::i(4, 1, 4, true), r::beq(4, 0, -4),
        r::i(5, 1, 0, true), r::sw(5, 1, 8), r::beq(0, 0, 0)}, cpu_config);
    system.cpu().set_register(1, 0x800);
    system.memory().write(0x800, c::word_bytes(7)); retire(system);
    assert(system.cpu().registers().read(3) == 7);
    c::DmaEngine dma(system.bus(), 7);
    auto payload = c::word_bytes(0xABCDEF01);
    const auto ready = c::word_bytes(1); payload.insert(payload.end(), ready.begin(), ready.end());
    // DMA 经 IOMMU 翻译后的物理写入，也必须被监听。
    system.iommu().map({7, 0x100000000ULL, 0x800, 32, true, true}); system.iommu().enable();
    dma.start({0x100000000ULL, 8, c::DmaDirection::DeviceToMemory}, payload);
    unsigned ticks = 0;
    while ((dma.busy() || c::load32(system.memory().read(0x808, 4)) != 0xABCDEF01) && ticks++ < 10000) {
        system.tick(); dma.tick();
    }
    assert(ticks < 10000 && dma.completed() && system.cpu().registers().read(5) == 0xABCDEF01);
    assert(system.data_cache().statistics().fills == 1 && system.data_cache().statistics().read_hits > 0);
    // CPU 写穿后设备 DMA 读到新数据，无需额外 flush。
    dma.start({0x100000008ULL, 4, c::DmaDirection::MemoryToDevice});
    while (dma.busy() && ticks++ < 20000) { system.tick(); dma.tick(); }
    assert(dma.completed() && dma.data() == c::word_bytes(0xABCDEF01));
}

struct Harness {
    dimm::DIMM dimm{{1, {4, {2, {64, 128, 8}}}}};
    soc::controller::Memory memory;
    soc::controller::IOMMU iommu;
    Interconnect bus;
    Port port;
    explicit Harness(soc::cache::Config cfg = Port::default_config(), interconnect::pcie::Config fabric = {},
                     bool enabled = true, std::uint32_t base = 0, unsigned hit_cycles = 1)
        : memory(dimm, base), bus(iommu, fabric), port(bus, cfg, enabled, hit_cycles) {
        bus.map_memory(base, dimm.geometry().capacity_bytes(), memory);
        memory.set_write_observer([this](auto address, const auto& data) { port.snoop_write(address, data); });
    }
    void tick() { bus.tick(); port.tick(); }
    c::BusResult finish(c::Tag tag) {
        unsigned ticks = 0;
        while (!port.ready(tag) && ticks++ < 10000) tick();
        assert(port.ready(tag)); return port.take(tag);
    }
    c::Bytes read(std::uint32_t address) {
        const auto result = finish(port.submit(0, c::BusOperation::Read, address, 4));
        assert(result.status == Status::Success); return result.data;
    }
};

static void configurable_hit_latency() {
    for (unsigned latency : {1u, 2u}) {
        Harness h(Port::default_config(), {}, true, 0, latency);
        h.memory.write(0x100, c::word_bytes(7));
        assert(c::load32(h.read(0x100)) == 7);
        const auto completed = h.bus.fabric().statistics().completed;
        const auto tag = h.port.submit(0, c::BusOperation::Read, 0x100, 4);
        assert(h.port.ready(tag) == (latency == 1));
        if (latency == 2) {
            throws<std::length_error>([&] { h.port.submit(0, c::BusOperation::Read, 0x100, 4); });
            // A write snooped before the data beat must be visible to that beat.
            h.memory.write(0x100, c::word_bytes(99));
            h.tick();
        }
        assert(h.port.ready(tag) && c::load32(h.port.take(tag).data) == (latency == 1 ? 7u : 99u));
        assert(h.bus.fabric().statistics().completed == completed);
    }
    throws<std::invalid_argument>([] { Harness invalid(Port::default_config(), {}, true, 0, 3); });
    for (auto cfg : cpu_configs()) {
        cfg.data_cache_hit_cycles = 2;
        cfg.enable_timed_dram = true;
        SoC system({r::i(3, 1, 0, true), r::i(4, 1, 0, true)}, cfg);
        system.cpu().set_register(1, 0x100); system.memory().write(0x100, c::word_bytes(42));
        bool delayed_hit = false;
        for (unsigned n = 0; n < 10000 && system.cpu().completed_instructions() < 2; ++n) {
            system.tick();
            if (cfg.cpu_execution == soc::RV32Core::Execution::Pipelined) {
                const auto& trace = *system.cpu().last_pipeline_cycle();
                delayed_hit |= trace.memory_stall && trace.pc[3] && *trace.pc[3] == 4;
            } else delayed_hit |= system.cpu().pc() == 4 && system.cpu().last_cycle()->bus_wait;
        }
        assert(system.cpu().completed_instructions() == 2 && system.cpu().registers().read(4) == 42 && delayed_hit &&
               system.dram_timing().statistics().reads > 0);
    }
}

static void fill_dma_race_and_backpressure() {
    Harness h;
    h.memory.write(0x100, c::word_bytes(7));
    const auto load = h.port.submit(0, c::BusOperation::Read, 0x100, 4);
    unsigned ticks = 0;
    do { h.tick(); assert(++ticks < 1000); }
    while (h.bus.fabric().last_tick().phase != interconnect::pcie::Phase::Data);
    // 首字已作为旧值7返回；缓存行其余数据拍尚在途中。
    const auto dma = h.bus.submit(9, c::BusOperation::Write, 0x100, 4, c::word_bytes(99));
    const auto result = h.finish(load);
    while (!h.bus.ready(dma) && ticks++ < 1000) h.tick();
    assert(h.bus.take(dma).status == Status::Success);
    assert(result.status == Status::Success && c::load32(result.data) == 99);
    assert(h.port.statistics().fill_forwarded_bytes == 4);
    const auto reads = h.memory.statistics().read_requests;
    assert(c::load32(h.read(0x100)) == 99 && h.memory.statistics().read_requests == reads);

    interconnect::pcie::Config one; one.max_outstanding = 1;
    Harness full(Port::default_config(), one);
    full.read(0x100);
    const auto occupying = full.bus.submit(9, c::BusOperation::Read, 0x200, 4);
    const auto hit = full.port.submit(0, c::BusOperation::Read, 0x100, 4);
    assert(full.port.ready(hit) && full.port.take(hit).status == Status::Success); // 命中不争抢总线。
    throws<std::length_error>([&] { full.port.submit(0, c::BusOperation::Read, 0x200, 4); });
    assert(full.port.cache().statistics().fills == 1);
    while (!full.bus.ready(occupying)) full.tick();
    full.bus.take(occupying);
    full.read(0x200);
    assert(full.port.cache().statistics().fills == 2 && full.port.cache().statistics().read_misses == 2);
}

static void store_after_earlier_snoop() {
    Harness h;
    h.memory.write(0x100, c::word_bytes(0));
    h.read(0x100); // The following store hits a resident line.
    const auto tag = h.port.submit(0, c::BusOperation::Write, 0x100, 4, c::word_bytes(0x11223344));
    // The other write commits first; the CPU write must win on that byte.
    h.memory.write(0x100, {0xee});
    assert(h.finish(tag).status == Status::Success);
    assert(c::load32(h.memory.inspect(0x100, 4)) == 0x11223344);
    assert(c::load32(h.read(0x100)) == 0x11223344);
    // A bus without an observer still updates a write-hit on acknowledgement.
    h.memory.set_write_observer({});
    const auto unobserved = h.port.submit(0, c::BusOperation::Write, 0x100, 4, c::word_bytes(0xaabbccdd));
    assert(h.finish(unobserved).status == Status::Success);
    assert(c::load32(h.read(0x100)) == 0xaabbccdd);
}

static void replacement_boundaries_and_errors() {
    auto cfg = Port::default_config(); cfg.sets = 1; cfg.ways = 1;
    Harness h(cfg);
    h.memory.write(0x100, c::word_bytes(11)); h.memory.write(0x120, c::word_bytes(22));
    assert(c::load32(h.read(0x100)) == 11 && c::load32(h.read(0x120)) == 22 && c::load32(h.read(0x100)) == 11);
    assert(h.port.cache().statistics().evictions == 2 && h.port.cache().statistics().writebacks == 0);
    Harness edge(cfg, {}, true, 4); // 首行跨越未映射区域，应按原始字请求绕过 Cache。
    assert(edge.read(4) == c::Bytes(4));
    assert(edge.port.statistics().bypasses == 1 && edge.memory.statistics().bytes_read == 4);
    const auto error = h.finish(h.port.submit(0, c::BusOperation::Read, 0x40000000, 4));
    assert(error.status == Status::Unmapped && error.transferred_bytes == 0);
    throws<std::out_of_range>([&] { h.port.submit(0, c::BusOperation::Read, 0x100000000ULL, 4); });
    throws<std::invalid_argument>([&] { h.port.submit(1, c::BusOperation::Read, 0, 4); });
    // 窗口登记为32B，实体只有16B：前四拍返回后失败，不得装入残缺行。
    dimm::DIMM tiny({1, {4, {1, {1, 4, 8}}}});
    soc::controller::Memory short_memory(tiny);
    soc::controller::IOMMU short_iommu;
    Interconnect short_bus(short_iommu); short_bus.map_memory(0, 32, short_memory);
    Port short_port(short_bus);
    const auto partial = short_port.submit(0, c::BusOperation::Read, 0, 4);
    unsigned ticks = 0;
    while (!short_port.ready(partial) && ticks++ < 1000) { short_bus.tick(); short_port.tick(); }
    const auto failed = short_port.take(partial);
    assert(failed.status == Status::BackendError && failed.data.empty() && failed.transferred_bytes == 0);
    assert(short_memory.statistics().bytes_read == 16 && !short_port.cache().contains(0, 4));
    interconnect::pcie::Config timeout; timeout.timing = interconnect::pcie::Timing::Synchronous;
    Harness slow(cfg, timeout);
    const auto timed = slow.finish(slow.port.submit(0, c::BusOperation::Read, 0x100, 4));
    assert(timed.status == Status::Timeout && !slow.port.cache().contains(0x100, 4));
    Harness disabled(cfg, {}, false);
    disabled.read(0x100); disabled.read(0x100);
    assert(disabled.port.cache().statistics().fills == 0 && disabled.memory.statistics().bytes_read == 8);
    cfg.write_policy = soc::cache::WritePolicy::WriteBack;
    throws<std::invalid_argument>([&] { Harness bad(cfg); });
    cfg = Port::default_config(); cfg.line_bytes = 2;
    throws<std::invalid_argument>([&] { Harness bad(cfg); });
    cfg = Port::default_config(); cfg.write_buffer_entries = 1;
    throws<std::invalid_argument>([&] { Harness bad(cfg); });
}

static void compare_cached_and_uncached(SoCConfig cpu_config, interconnect::pcie::Timing timing) {
    // 同一真实机器码分别走缓存/直连，与每条退休后的架构状态及最终 DIMM 做对照。
    std::mt19937 random(20261115);
    std::vector<std::uint32_t> program;
    for (unsigned n = 0; n < 300; ++n) {
        const auto kind = random() % 3;
        if (kind == 0) { program.push_back(r::i(2, 2, 1)); continue; }
        const unsigned loads[]{0, 1, 2, 4, 5};
        const auto funct3 = kind == 1 ? random() % 3 : loads[random() % 5];
        const auto bytes = 1u << (funct3 & 3u);
        const auto offset = static_cast<std::uint32_t>((random() % 128) / bytes * bytes);
        if (kind == 1) {
            program.push_back(((offset >> 5) << 25) | (2u << 20) | (1u << 15) |
                (funct3 << 12) | ((offset & 31u) << 7) | 0x23u);
        } else {
            const auto rd = 3 + random() % 12;
            program.push_back((offset << 20) | (1u << 15) | (funct3 << 12) | (rd << 7) | 3u);
        }
    }
    auto cfg = cpu_config;
    cfg.data_cache.sets = 2; cfg.data_cache.ways = 2; cfg.data_cache.line_bytes = 16;
    cfg.fabric.timing = timing; cfg.fabric.synchronous_wait = 2;
    SoC cached(program, cfg);
    cfg.enable_data_cache = false;
    SoC uncached(program, cfg);
    cached.cpu().set_register(1, 0x1000); uncached.cpu().set_register(1, 0x1000);
    cached.memory().write(0x1000, pattern(128)); uncached.memory().write(0x1000, pattern(128));
    for (std::size_t n = 0; n < program.size(); ++n) {
        retire(cached); retire(uncached);
        assert(cached.cpu().pc() == uncached.cpu().pc());
        for (unsigned reg = 0; reg < 32; ++reg)
            assert(cached.cpu().registers().read(reg) == uncached.cpu().registers().read(reg));
        const auto& a = cached.cpu().last_retirement()->memory_write;
        const auto& b = uncached.cpu().last_retirement()->memory_write;
        assert(bool(a) == bool(b));
        if (a) assert(a->address == b->address && a->value == b->value && a->bytes == b->bytes);
    }
    assert(cached.memory().read(0x1000, 128) == uncached.memory().read(0x1000, 128));
    assert(cached.data_cache().statistics().read_hits > 0 && cached.data_cache().statistics().evictions > 0);
    assert(uncached.data_cache().statistics().fills == 0);
}

int main() {
    for (const auto& cpu_config : cpu_configs()) {
        cpu_hit_miss_and_stores(cpu_config); mmu_before_cache(cpu_config); mmio_is_uncached(cpu_config);
        pmp_before_cache(cpu_config); dma_coherence(cpu_config);
        for (const auto timing : {interconnect::pcie::Timing::Synchronous, interconnect::pcie::Timing::Asynchronous,
             interconnect::pcie::Timing::SemiSynchronous, interconnect::pcie::Timing::Split})
            compare_cached_and_uncached(cpu_config, timing);
    }
    configurable_hit_latency(); fill_dma_race_and_backpressure(); store_after_earlier_snoop(); replacement_boundaries_and_errors();
    std::cout << "PASS CPU Cache: RV32 pipeline + both multi-cycle controllers, MMU protection, hit/miss timing, MMIO bypass, DMA snooping/races, faults\n";
}
