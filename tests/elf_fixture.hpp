#pragma once
#include "../devices/common/mmio_device.hpp"

inline colab::devices::common::Bytes elf_fixture() {
    using namespace colab::devices::common;
    Bytes bytes(0x204, 0);
    bytes[0] = 0x7f; bytes[1] = 'E'; bytes[2] = 'L'; bytes[3] = 'F';
    bytes[4] = 1; bytes[5] = 1; bytes[6] = 1;
    bytes[16] = 2; bytes[18] = 243; bytes[20] = 1;
    store32(bytes, 24, 0x8000); store32(bytes, 28, 52);
    bytes[40] = 52; bytes[42] = 32; bytes[44] = 2;
    store32(bytes, 52, 1); store32(bytes, 56, 0x100); store32(bytes, 60, 0x8000); store32(bytes, 64, 0x8000);
    store32(bytes, 68, 12); store32(bytes, 72, 12); store32(bytes, 76, 5); store32(bytes, 80, 4);
    store32(bytes, 84, 1); store32(bytes, 88, 0x200); store32(bytes, 92, 0x200); store32(bytes, 96, 0x200);
    store32(bytes, 100, 4); store32(bytes, 104, 16); store32(bytes, 108, 6); store32(bytes, 112, 4);
    store32(bytes, 0x100, 0x20002083); // lw x1, 0x200(x0)
    store32(bytes, 0x104, 0x06102023); // sw x1, 0x60(x0)
    store32(bytes, 0x108, 0x00000063); // beq x0, x0, 0
    store32(bytes, 0x200, 0xcafebabe);
    return bytes;
}
