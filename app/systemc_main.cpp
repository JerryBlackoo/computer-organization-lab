#include "../simulation/systemc/platform.hpp"
#include "../examples/guest/io_driver.hpp"
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <poll.h>
#include <sstream>
#include <unistd.h>

namespace sc = colab::simulation::systemc;
namespace common = colab::devices::common;
using namespace colab;

static std::uint32_t number(const std::string& text) {
    const bool hex = text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
    const auto first = text.data() + (hex ? 2 : 0);
    const auto last = text.data() + text.size();
    std::uint32_t value = 0;
    const auto result = std::from_chars(first, last, value, hex ? 16 : 10);
    if (first == last || result.ec != std::errc{} || result.ptr != last)
        throw std::invalid_argument("expected a 32-bit decimal/hex number");
    return value;
}
static void status(sc::Platform& machine) {
    std::cout << "power=" << (machine.powered() ? "on" : "off") << " time=" << sc_core::sc_time_stamp()
              << " cycles=" << machine.cycles.read() << " pc=0x" << std::hex << machine.cpu().pc() << std::dec
              << " retired=" << machine.cpu().completed_instructions() << " IRQs=" << machine.cpu().interrupts().entries()
              << " exceptions=" << machine.cpu().interrupts().exceptions()
              << " mode=" << (machine.cpu().interrupts().privilege() == soc::core::PrivilegeMode::Machine ? 'M' :
                  machine.cpu().interrupts().privilege() == soc::core::PrivilegeMode::Supervisor ? 'S' : 'U')
              << " waiting=" << (machine.cpu().waiting() ? "wfi" : "no")
              << " WFI cycles=" << machine.cpu().wfi_wait_cycles()
              << " bus=" << (machine.bus().idle() ? "idle" : "active")
              << " I-cache fills=" << machine.instruction_cache().cache().statistics().fills
              << " D-cache fills=" << machine.data_cache().cache().statistics().fills
              << " NVMe commands=" << machine.nvme().completed_commands()
              << " HDD DMA bytes=" << machine.hdd().dma().transferred()
              << " timer count=" << machine.timer().mmio_read32(devices::Timer::Count)
              << " timer pending=" << machine.timer().mmio_read32(devices::Timer::Pending)
              << " mtime=" << machine.clint().time() << " timebase-hz=" << machine.timebase_hz()
              << " uart status=" << machine.uart().mmio_read32(devices::Uart::Status)
              << " Sv32 walks=" << machine.cpu().sv32().statistics().walks
              << " PTE reads=" << machine.cpu().sv32().statistics().reads
              << " page faults=" << machine.cpu().sv32().statistics().page_faults << '\n';
    if (!machine.fault().empty()) std::cout << machine.fault() << '\n';
}
static void flush_uart(sc::Platform& machine) {
    const auto output = machine.uart().take_output();
    if (!output.empty()) { std::cout << output << std::flush; }
}
static void interactive(sc::Platform& machine) {
    std::cout << "Commands: on, off, reset, run [cycles], pause, step [cycles], status, regs, csrs, mem <address> [bytes], uart-input <text>, quit\n"
              << "Machine starts powered off. 'run' continues until 'pause'.\n" << std::flush;
    bool running = false;
    std::string pending;
    while (true) {
        if (pending.find('\n') == std::string::npos) {
            pollfd input{STDIN_FILENO, POLLIN, 0};
            const auto ready = poll(&input, 1, running ? 0 : -1);
            if (ready < 0) { if (errno == EINTR) continue; throw std::runtime_error("stdin poll failed"); }
            if (!ready) {
                machine.run(64);
                flush_uart(machine);
                if (!machine.fault().empty()) { running = false; status(machine); }
                continue;
            }
            char buffer[4096];
            const auto count = read(STDIN_FILENO, buffer, sizeof(buffer));
            if (count <= 0) break;
            pending.append(buffer, static_cast<std::size_t>(count));
            continue;
        }
        const auto end = pending.find('\n');
        std::istringstream input(pending.substr(0, end));
        pending.erase(0, end + 1);
        std::string command, argument;
        input >> command >> argument;
        try {
            if (command == "quit" || command == "exit") break;
            if (command == "on") { running = false; machine.power_on(); status(machine); }
            else if (command == "off") { running = false; machine.power_off(); status(machine); }
            else if (command == "reset") { running = false; machine.reset(); status(machine); }
            else if (command == "pause") { running = false; machine.pause(); status(machine); }
            else if (command == "run" || command == "step") {
                if (!machine.powered()) throw std::logic_error("Power On first");
                if (command == "run" && argument.empty()) running = true;
                else {
                    running = false;
                    // Drain in bounded batches to avoid holding up the simulated UART indefinitely.
                    auto left = argument.empty() ? 1u : number(argument);
                    while (left) { const auto batch = std::min(left, 64u); machine.run(batch); flush_uart(machine); left -= batch; }
                    status(machine);
                }
            } else if (command == "status") status(machine);
            else if (command == "uart-input") {
                if (!machine.powered()) throw std::logic_error("Power On first");
                std::string rest; std::getline(input, rest);
                machine.uart().inject(argument + rest);
            }
            else if (command == "regs") {
                for (unsigned i = 0; i < 32; ++i)
                    std::cout << 'x' << std::dec << i << "=0x" << std::hex << machine.cpu().registers().read(i)
                              << (i % 4 == 3 ? '\n' : ' ');
                std::cout << std::dec;
            } else if (command == "csrs") {
                const std::pair<const char*, unsigned> registers[]{
                    {"mstatus", 0x300}, {"misa", 0x301}, {"mie", 0x304}, {"mtvec", 0x305}, {"mscratch", 0x340},
                    {"mepc", 0x341}, {"mcause", 0x342}, {"mtval", 0x343}, {"mip", 0x344},
                    {"medeleg", 0x302}, {"mideleg", 0x303}, {"sstatus", 0x100}, {"sie", 0x104}, {"sip", 0x144},
                    {"stvec", 0x105}, {"sscratch", 0x140}, {"sepc", 0x141}, {"scause", 0x142}, {"stval", 0x143}, {"satp", 0x180},
                    {"pmpcfg0", 0x3a0}, {"pmpaddr0", 0x3b0}, {"pmpaddr1", 0x3b1}, {"pmpaddr2", 0x3b2}, {"mhartid", 0xf14}};
                for (const auto& entry : registers)
                    std::cout << entry.first << "=0x" << std::hex << machine.cpu().interrupts().read(entry.second) << ' ';
                std::cout << std::dec << '\n';
            } else if (command == "mem") {
                std::string length; input >> length;
                const auto address = number(argument), count = length.empty() ? 32 : number(length);
                if (count > 1024) throw std::invalid_argument("memory view is limited to 1024 bytes");
                const auto bytes = machine.memory().inspect(address, count);
                for (std::size_t i = 0; i < bytes.size(); ++i) {
                    if (i % 16 == 0) std::cout << "0x" << std::hex << address + i << ": ";
                    std::cout << std::setfill('0') << std::setw(2) << unsigned(bytes[i]) << (i % 16 == 15 ? '\n' : ' ');
                }
                std::cout << std::dec << std::setfill(' ') << '\n';
            } else if (!command.empty()) std::cout << "Unknown command: " << command << '\n';
        } catch (const std::exception& error) { std::cout << "Command failed: " << error.what() << '\n'; }
        std::cout << std::flush;
    }
}
static sc::BootImage load_binary(const std::string& path, std::uint32_t entry, std::uint64_t ram_size) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const std::streamoff size = file.tellg();
    if (!file || size <= 0 || size % 4) throw std::runtime_error("raw RV32 image must be at least 4 bytes, multiple of 4");
    // entry is both the load address and the initial PC. Check before allocating,
    // using subtraction so neither the file size nor the end address can wrap.
    const auto length = static_cast<std::uintmax_t>(size);
    const auto address_space = std::uint64_t{std::numeric_limits<std::uint32_t>::max()} + 1;
    const auto limit = std::min(ram_size, address_space);
    if (entry % 4 || entry >= limit || length > limit - entry ||
        length > std::numeric_limits<std::size_t>::max() ||
        length > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()))
        throw std::runtime_error("boot text is outside DIMM");
    common::Bytes bytes(static_cast<std::size_t>(length));
    file.seekg(0); file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(length));
    if (!file) throw std::runtime_error("cannot read binary image");
    if (bytes[0] == 0x7F && bytes[1] == 'E' && bytes[2] == 'L' && bytes[3] == 'F')
        throw std::runtime_error("ELF is not a flat binary; use --elf instead of --bin");
    sc::BootImage image; image.entry = entry;
    for (std::size_t i = 0; i < bytes.size(); i += 4) image.text.push_back(common::load32(bytes, i));
    return image;
}
static sc::BootImage load_elf(const std::string& path, std::uint64_t ram_size) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto size = file.tellg();
    if (!file || size <= 0 || size > 4 * 1024 * 1024) throw std::runtime_error("invalid ELF file size");
    common::Bytes bytes(static_cast<std::size_t>(size));
    file.seekg(0); file.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!file) throw std::runtime_error("cannot read ELF image");
    auto elf = simulation::boot::parse_elf32(bytes, ram_size);
    sc::BootImage image; image.entry = elf.entry; image.segments = std::move(elf.segments);
    return image;
}

int sc_main(int argc, char** argv) {
    try {
        std::string demo = "nvme", binary, elf_path, trace_path;
        std::uint32_t entry = 0x8000, limit = 200000;
        bool console = false, entry_supplied = false, dram_banks_supplied = false;
        SoCConfig config; config.disk.spin_up_ms = 1;
        for (int i = 1; i < argc; ++i) {
            const std::string flag = argv[i];
            if (flag == "--interactive") { console = true; continue; }
            if (flag == "--timed-dram") { config.enable_timed_dram = true; continue; }
            if (flag == "--interleaved-dram") { config.dram_mode = DramScheduleMode::InterleavedPipeline; continue; }
            if (flag == "--dram-no-bank-check") { config.interleaved_dram.require_ta_equals_n_times_bus = false; continue; }
            if (flag == "--help") {
                std::cout << "systemc_machine [--interactive] [--demo nvme|hdd] [--trace VCD-stem]\n"
                          << "                [--bin RV32.bin --entry 0x8000 | --elf static-RV32.elf] [--cycles 200000]\n"
                          << "                [--timed-dram] [--dram-trcd N] [--dram-trp N] [--dram-tcas N]\n"
                          << "                [--interleaved-dram] [--dram-banks N] [--dram-ta N] [--dram-bus N]\n"
                          << "                [--dram-interleaved-trp N] [--dram-no-bank-check]\n"
                          << "                [--icache-hit-cycles 1|2] [--dcache-hit-cycles 1|2]\n";
                return 0;
            }
            if (++i >= argc) throw std::invalid_argument("missing option value");
            if (flag == "--demo") demo = argv[i];
            else if (flag == "--bin") binary = argv[i];
            else if (flag == "--elf") elf_path = argv[i];
            else if (flag == "--entry") { entry = number(argv[i]); entry_supplied = true; }
            else if (flag == "--cycles") limit = number(argv[i]);
            else if (flag == "--trace") trace_path = argv[i];
            else if (flag == "--dram-trcd") config.dram_timing.tRCD = number(argv[i]);
            else if (flag == "--dram-trp") config.dram_timing.tRP = number(argv[i]);
            else if (flag == "--dram-tcas") config.dram_timing.tCAS = number(argv[i]);
            else if (flag == "--dram-ta") config.interleaved_dram.ta = number(argv[i]);
            else if (flag == "--dram-bus") config.interleaved_dram.t_bus = number(argv[i]);
            else if (flag == "--dram-interleaved-trp") config.interleaved_dram.tRP = number(argv[i]);
            else if (flag == "--dram-banks") { config.memory.rank.dram_chip.banks = number(argv[i]); dram_banks_supplied = true; }
            else if (flag == "--icache-hit-cycles") config.instruction_cache_hit_cycles = number(argv[i]);
            else if (flag == "--dcache-hit-cycles") config.data_cache_hit_cycles = number(argv[i]);
            else throw std::invalid_argument("unknown option " + flag);
        }
        if (config.dram_mode == DramScheduleMode::InterleavedPipeline && !dram_banks_supplied)
            config.memory.rank.dram_chip.banks = 4;
        (void)config.selected_dram_mode();
        if (demo != "nvme" && demo != "hdd") throw std::invalid_argument("demo must be nvme or hdd");
        if (!binary.empty() && !elf_path.empty()) throw std::invalid_argument("choose either --bin or --elf");
        if (!elf_path.empty() && entry_supplied) throw std::invalid_argument("ELF supplies its own entry; omit --entry");
        const bool custom_image = !binary.empty() || !elf_path.empty();
        sc::BootImage boot;
        if (!binary.empty()) boot = load_binary(binary, entry, config.memory.capacity_bytes());
        else if (!elf_path.empty()) boot = load_elf(elf_path, config.memory.capacity_bytes());
        else {
            const auto image = (demo == "nvme" ? examples::guest::nvme_read()
                : examples::guest::hdd_read(examples::guest::IoMode::DmaInterrupt)).relocated(entry);
            boot = {image.entry_point, image.instructions, image.constants};
        }
        sc::Platform machine("computer", std::move(boot), sc_core::sc_time(50, sc_core::SC_US), config);
        common::Bytes expected(512);
        for (std::size_t i = 0; i < expected.size(); ++i) expected[i] = static_cast<std::uint8_t>(i * 73 + 5);
        if (!custom_image) {
            if (demo == "nvme") machine.nand().write(0, expected);
            else {
                machine.disk().power_on(); machine.disk().advance(1);
                machine.disk().submit_write_lba(0, expected);
                if (!machine.disk().run_until_idle(100)) throw std::runtime_error("HDD image initialization timed out");
                const auto result = machine.disk().take_completion();
                if (!result || result->status != devices::hdd::Status::Success) throw std::runtime_error("HDD image initialization failed");
                machine.disk().power_off();
            }
        }
        std::unique_ptr<sc_core::sc_trace_file, decltype(&sc_core::sc_close_vcd_trace_file)>
            trace(nullptr, sc_core::sc_close_vcd_trace_file);
        if (!trace_path.empty()) {
            trace.reset(sc_core::sc_create_vcd_trace_file(trace_path.c_str()));
            if (!trace) throw std::runtime_error("cannot create trace");
            trace->set_time_unit(1, sc_core::SC_NS); machine.trace(trace.get());
        }
        if (console) { interactive(machine); return 0; }
        machine.power_on();
        while (machine.cycles.read() < limit && machine.fault().empty()) {
            if (!custom_image && common::load32(machine.memory().inspect(0x10, 4)) == 1) break;
            machine.step();
            flush_uart(machine);
        }
        status(machine);
        if (!machine.fault().empty()) return 1;
        if (!custom_image) {
            if (common::load32(machine.memory().inspect(0x10, 4)) != 1 ||
                machine.memory().inspect(demo == "nvme" ? 0x5000 : 0x800, 512) != expected)
                throw std::runtime_error("guest I/O incomplete or data differs");
            std::cout << "PASS: real RV32 guest + instruction/data Cache + TLM fabric + DMA + DIMM"
                      << (demo == "hdd" ? " + IRQ/ISR/MRET" : " + NVMe queues") << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
