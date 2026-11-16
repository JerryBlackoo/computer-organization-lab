#include "rv32_test_support.hpp"

using namespace rv32_test;

int main() {
    colab::soc::core::Decoder decoder;
    // 固定机器字独立于测试编码器：用户要求的原始验收程序。
    const auto addi = decoder.decode_rv32(0x00A00093);
    assert(addi.operation == Operation::Addi && addi.rd == 1 && addi.rs1 == 0 && addi.immediate == 10);
    assert(addi.raw == 0x00A00093 && addi.opcode == 0x13 && addi.funct3 == 0);
    const auto add = decoder.decode_rv32(0x002081B3);
    assert(add.operation == Operation::Add && add.rd == 3 && add.rs1 == 1 && add.rs2 == 2);
    const auto sub = decoder.decode_rv32(0x402081B3);
    assert(sub.operation == Operation::Sub && sub.funct7 == 0x20);
    for (int imm = -2048; imm <= 2047; ++imm) {
        const auto immediate = decoder.decode_rv32(i(31, 30, imm));
        assert(immediate.immediate == imm && immediate.rd == 31 && immediate.rs1 == 30);
        const auto load = decoder.decode_rv32(i(0, 31, imm, true));
        assert(load.operation == Operation::Lw && load.immediate == imm && load.rd == 0 && load.rs1 == 31);
        const auto store = decoder.decode_rv32(sw(29, 28, imm));
        assert(store.operation == Operation::Sw && store.immediate == imm && store.rs1 == 28 && store.rs2 == 29);
    }
    for (int imm = -4096; imm <= 4094; imm += 2) {
        const auto branch = decoder.decode_rv32(beq(31, 30, imm));
        assert(branch.operation == Operation::Beq && branch.immediate == imm && branch.rs1 == 31 && branch.rs2 == 30);
    }
    for (unsigned rd = 0; rd < 32; ++rd)
        for (unsigned rs = 0; rs < 32; ++rs) {
            const auto decoded = decoder.decode_rv32(r(rd, rs, 31));
            assert(decoded.rd == rd && decoded.rs1 == rs && decoded.rs2 == 31);
        }
    for (auto word : {0u, 0xFFFFFFFFu, 0x022081B3u, 0x022091B3u, 0x02001013u,
                      0x00003003u, 0x00003023u, 0x00002063u, 0x00200073u})
        expect_throw<std::invalid_argument>([&] { decoder.decode_rv32(word); });
    // 旧 16 位 Decoder 的接口保持可用。
    const auto legacy = decoder.decode(0x2345);
    assert(legacy.op == colab::isa::Opcode::Sub && legacy.rd == 3);
}
