#pragma once

#include "../../soc/soc.hpp"
#include <map>

namespace colab::examples::guest {
using Block = devices::hdd::IOController<devices::HDD>;
enum class IoMode { Polling, Interrupt, DmaPolling, DmaInterrupt };
struct Image {
    std::vector<std::uint32_t> instructions;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> constants;
    std::uint32_t done_pc = 0;
    std::uint32_t entry_point = 0;
    std::vector<std::pair<std::size_t, std::uint32_t>> code_addresses;
    void load_constants(soc::controller::Memory& memory) const {
        for (auto [address, value] : constants) memory.write(address, devices::common::word_bytes(value));
    }
    // Branches are PC-relative; li_label fixups need absolute relocation.
    Image relocated(std::uint32_t base) const {
        if (entry_point || base % 4 || instructions.size() > (UINT32_MAX - base) / 4)
            throw std::invalid_argument("invalid/repeated image relocation");
        Image result = *this;
        result.entry_point = base;
        result.done_pc += base;
        for (const auto& [index, address] : code_addresses) {
            const auto slot = 0x100u + static_cast<std::uint32_t>(result.constants.size() * 4);
            if (slot >= 0x300) throw std::length_error("relocation constant pool exhausted");
            const auto rd = (result.instructions.at(index) >> 7) & 31u;
            result.instructions[index] = (slot << 20) | 0x2003u | (rd << 7);
            result.constants.push_back({slot, base + address});
        }
        return result;
    }
    void load_text(soc::controller::Memory& memory) const {
        for (std::size_t i = 0; i < instructions.size(); ++i)
            memory.write(entry_point + static_cast<std::uint32_t>(i * 4), devices::common::word_bytes(instructions[i]));
    }
};

// A tiny assembler for the existing ISA subset, not a host-side device driver.
// Large immediates live in guest .rodata, loaded by real LW instructions.
class Assembler {
public:
    void label(std::string name) { labels_[std::move(name)] = static_cast<std::uint32_t>(image_.instructions.size() * 4); }
    void emit(std::uint32_t word) { image_.instructions.push_back(word); }
    void addi(unsigned rd, unsigned rs, int imm) {
        if (imm < -2048 || imm > 2047) throw std::out_of_range("ADDI immediate");
        emit(((std::uint32_t(imm) & 4095u) << 20) | (rs << 15) | (rd << 7) | 0x13u);
    }
    void add(unsigned rd, unsigned a, unsigned b) { emit((b << 20) | (a << 15) | (rd << 7) | 0x33u); }
    void lw(unsigned rd, unsigned base, unsigned offset) {
        if (offset > 2047) throw std::out_of_range("LW offset");
        emit((offset << 20) | (base << 15) | 0x2000u | (rd << 7) | 3u);
    }
    void sw(unsigned rs, unsigned base, unsigned offset) {
        if (offset > 2047) throw std::out_of_range("SW offset");
        emit(((offset >> 5) << 25) | (rs << 20) | (base << 15) | 0x2000u | ((offset & 31u) << 7) | 0x23u);
    }
    void li(unsigned rd, std::uint32_t value) {
        if (value <= 2047) { addi(rd, 0, static_cast<int>(value)); return; }
        const auto address = 0x100u + static_cast<std::uint32_t>(image_.constants.size() * 4);
        if (address >= 0x300) throw std::length_error("guest constant pool exhausted");
        image_.constants.push_back({address, value}); lw(rd, 0, address);
    }
    void li_label(unsigned rd, std::string label) {
        address_fixups_.push_back({image_.instructions.size(), std::move(label)});
        li(rd, 0);
    }
    void beq(unsigned a, unsigned b, std::string label) {
        branches_.push_back({image_.instructions.size(), std::move(label)});
        emit((b << 20) | (a << 15) | 0x63u);
    }
    void csr(unsigned rd, unsigned rs, unsigned number, bool set = false) {
        emit((number << 20) | (rs << 15) | (set ? 0x2000u : 0x1000u) | (rd << 7) | 0x73u);
    }
    void store_value(unsigned base, unsigned offset, std::uint32_t value) { li(5, value); sw(5, base, offset); }
    Image finish() {
        for (const auto& fixup : branches_) {
            const auto offset = std::int64_t(labels_.at(fixup.label)) - std::int64_t(fixup.index * 4);
            if (offset < -4096 || offset > 4094) throw std::out_of_range("guest branch distance");
            const auto bits = std::uint32_t(offset) & 8191u;
            image_.instructions[fixup.index] |= ((bits >> 12) << 31) | (((bits >> 5) & 63u) << 25)
                | (((bits >> 1) & 15u) << 8) | (((bits >> 11) & 1u) << 7);
        }
        for (const auto& fixup : address_fixups_) {
            const auto address = labels_.at(fixup.label);
            if (address > 2047) throw std::out_of_range("guest trap handler address");
            image_.instructions[fixup.index] |= address << 20;
            image_.code_addresses.push_back({fixup.index, address});
        }
        image_.done_pc = labels_.at("done");
        return image_;
    }
private:
    struct Fixup { std::size_t index; std::string label; };
    Image image_;
    std::map<std::string, std::uint32_t> labels_;
    std::vector<Fixup> branches_, address_fixups_;
};

inline Image hdd_read(IoMode mode, std::uint32_t destination = 0x800, std::uint32_t length = 512) {
    if (!length || length % 4 || length > 1024) throw std::invalid_argument("guest HDD driver word count");
    const bool dma = mode == IoMode::DmaPolling || mode == IoMode::DmaInterrupt;
    const bool interrupt = mode == IoMode::Interrupt || mode == IoMode::DmaInterrupt;
    Assembler a;
    a.li(1, SoC::HddBase); a.li(10, SoC::IrqBase); a.li(2, 0x400);
    a.li(12, destination); a.li(26, destination); a.li(13, length / 4); a.li(6, Block::Ready); a.li(4, 1);
    if (interrupt) {
        a.li_label(5, "handler"); a.csr(0, 5, 0x305);
        a.li(5, 1u << 11); a.csr(0, 5, 0x304);
        a.li(5, 8); a.csr(0, 5, 0x300);
    }
    a.store_value(1, Block::Lba, 0); a.store_value(1, Block::Length, length);
    a.store_value(1, Block::Control, (dma ? Block::EnableDma : 0) | (interrupt ? Block::EnableIrq : 0));
    if (dma) a.sw(12, 1, Block::DmaAddress);
    a.sw(4, 1, Block::Command);
    a.label("wait");
    if (interrupt) {
        a.addi(9, 9, 1); // Independent useful work while the controller/DMA runs.
        a.beq(31, 0, "wait");
    } else if (dma) {
        a.li(7, Block::Done); a.lw(5, 1, Block::Status); a.beq(5, 7, "finish");
        a.addi(9, 9, 1); a.beq(0, 0, "wait");
    } else {
        a.lw(5, 1, Block::Status); a.beq(5, 6, "copy"); a.beq(0, 0, "wait");
        a.label("copy"); a.lw(22, 1, Block::Data); a.sw(22, 12, 0);
        a.addi(12, 12, 4); a.addi(13, 13, -1); a.beq(13, 0, "finish"); a.beq(0, 0, "wait");
    }
    a.label("finish"); a.lw(28, 26, 0); a.sw(4, 0, 0x10);
    a.label("done"); a.beq(0, 0, "done");
    if (interrupt) {
        a.label("handler");
        a.addi(2, 2, -16); a.sw(20, 2, 0); a.sw(21, 2, 4); a.sw(22, 2, 8); a.sw(23, 2, 12);
        a.lw(21, 10, soc::controller::Interrupt::Claim); a.beq(21, 0, "restore");
        if (!dma) {
            a.lw(22, 1, Block::Data); a.sw(22, 12, 0); a.addi(12, 12, 4); a.addi(13, 13, -1);
            a.beq(13, 0, "last"); a.beq(0, 0, "ack");
            a.label("last");
        }
        a.li(31, 1);
        a.label("ack"); a.sw(4, 1, Block::IrqStatus); a.sw(21, 10, soc::controller::Interrupt::Complete);
        a.label("restore");
        a.lw(20, 2, 0); a.lw(21, 2, 4); a.lw(22, 2, 8); a.lw(23, 2, 12); a.addi(2, 2, 16);
        a.emit(0x30200073u); // MRET, with PC/MIE restored by the privileged state machine.
    }
    return a.finish();
}

inline Image nvme_read() {
    using C = devices::nvme::Controller;
    Assembler a;
    a.li(1, SoC::NvmeBase); a.li(14, 0x1000); a.li(15, 0x2000);
    a.li(16, 0x3000); a.li(17, 0x4000); a.li(12, 0x5000);
    a.li(5, 0x1000); a.add(18, 1, 5); // doorbell base
    a.store_value(1, C::AdminAttributes, 0x00030003); a.sw(14, 1, C::AdminSq); a.sw(15, 1, C::AdminCq);
    a.store_value(1, C::Config, 0x00460001);
    a.li(6, 1); a.label("ready"); a.lw(5, 1, C::Status); a.beq(5, 6, "create"); a.beq(0, 0, "ready");
    a.label("create");
    for (unsigned offset = 0; offset < 128; offset += 4) a.sw(0, 14, offset);
    a.store_value(14, 0, 0x00010005); a.sw(17, 14, 24); // Create IO CQ, CID=1.
    a.store_value(14, 40, 0x00030001); a.store_value(14, 44, 3); // QID=1, depth=4, PC/IEN.
    a.store_value(14, 64, 0x00020001); a.sw(16, 14, 88); // Create IO SQ, CID=2.
    a.store_value(14, 104, 0x00030001); a.store_value(14, 108, 0x00010001);
    a.store_value(18, 0, 2); a.li(6, 0x00010002);
    a.label("admin"); a.lw(5, 15, 28); a.beq(5, 6, "submit"); a.beq(0, 0, "admin");
    a.label("submit"); a.store_value(18, 4, 2);
    for (unsigned offset = 0; offset < 64; offset += 4) a.sw(0, 16, offset);
    a.store_value(16, 0, 0x00030002); a.store_value(16, 4, 1); a.sw(12, 16, 24); // Read NSID=1 LBA=0 NLB=0.
    a.store_value(18, 8, 1); a.li(6, 0x00010003);
    a.label("io"); a.addi(9, 9, 1); a.lw(5, 17, 12); a.beq(5, 6, "finish"); a.beq(0, 0, "io");
    a.label("finish"); a.store_value(18, 12, 1); a.lw(28, 12, 0); a.store_value(0, 0x10, 1);
    a.label("done"); a.beq(0, 0, "done");
    return a.finish();
}
} // namespace colab::examples::guest
