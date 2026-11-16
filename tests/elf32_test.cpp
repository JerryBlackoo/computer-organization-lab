#include "elf_fixture.hpp"
#include "../simulation/boot/elf32.hpp"
#include <iostream>

static void reject(const colab::devices::common::Bytes& data) {
    try { (void)colab::simulation::boot::parse_elf32(data, 65536); }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid ELF accepted");
}
int main() {
    using namespace colab::devices::common;
    const auto valid = elf_fixture();
    const auto image = colab::simulation::boot::parse_elf32(valid, 65536);
    if (image.entry != 0x8000 || image.segments.size() != 2 || image.segments[1].memory_size != 16 ||
        load32(image.segments[1].data) != 0xcafebabe) throw std::runtime_error("valid ELF was not decoded");
    auto file = valid; file.resize(40); reject(file);
    file = valid; file[4] = 2; reject(file); // ELF64
    file = valid; file[5] = 2; reject(file); // big endian
    file = valid; file[18] = 62; reject(file); // x86-64
    file = valid; file[16] = 3; reject(file); // ET_DYN
    file = valid; store32(file, 36, 1); reject(file); // compressed ISA flag
    file = valid; store32(file, 28, 0xfffffff0); reject(file); // truncated header table
    file = valid; store32(file, 68, 13); store32(file, 72, 12); reject(file); // filesz > memsz
    file = valid; store32(file, 56, 0x1000); reject(file); // segment outside file
    file = valid; store32(file, 60, 0xfffc); store32(file, 64, 0xfffc); reject(file); // segment outside RAM
    file = valid; store32(file, 92, 0x8000); store32(file, 96, 0x8000); reject(file); // overlap
    file = valid; store32(file, 24, 0x200); reject(file); // non-executable entry
    file = valid; store32(file, 64, 0x9000); reject(file); // virtual/physical mismatch
    file = valid; store32(file, 84, 3); reject(file); // PT_INTERP
    std::cout << "PASS ELF32 validation, executable entry, data and BSS geometry\n";
}
