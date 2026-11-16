typedef unsigned int word;
_Static_assert(sizeof(word) == 4, "RV32 requires 32-bit unsigned int");

volatile word inputs[4] = {0x12345678u, 0x80000000u, 0xffffffffu, 0x01020304u};
volatile word signature;
volatile unsigned char byte_echo[4];
volatile unsigned short half_echo[2];
volatile signed char signed_byte = -128;
volatile signed short signed_half = -32768;
volatile word signed_echo[2];
extern char compiler_smoke_ecall;
extern char compiler_smoke_wfi_after;

__attribute__((noinline)) static word timer_smoke(void) {
    volatile word* const time = (volatile word*)0x0200bff8;
    volatile word* const compare = (volatile word*)0x02004000;
    word old_mie, old_status, high, low;
    *(volatile word*)0x80 = 0;
    *(volatile word*)0x84 = 0;
    __asm__ volatile("csrr %0, mie\ncsrr %1, mstatus" : "=r"(old_mie), "=r"(old_status));
    do { high = time[1]; low = time[0]; } while (high != time[1]);
    const word deadline = low + 2048;
    compare[0] = 0xffffffff;
    compare[1] = high + (deadline < low);
    compare[0] = deadline;
    __asm__ volatile("csrs mie, %0\ncsrsi mstatus, 8" : : "r"(128u) : "memory");
    __asm__ volatile("wfi\n.globl compiler_smoke_wfi_after\ncompiler_smoke_wfi_after:" ::: "memory");
    __asm__ volatile("csrw mstatus, %0\ncsrw mie, %1" : : "r"(old_status), "r"(old_mie) : "memory");
    return *(volatile word*)0x80 == 1 && *(volatile word*)0x84 == (word)&compiler_smoke_wfi_after;
}

__attribute__((noinline)) static word csr_smoke(void) {
    word a, b, c, d, e, f, result;
    __asm__ volatile(
        "csrw mscratch, zero\n"
        "csrrw %0, mscratch, %7\n"
        "csrrs %1, mscratch, %8\n"
        "csrrc %2, mscratch, %9\n"
        "csrrwi %3, mscratch, 3\n"
        "csrrsi %4, mscratch, 16\n"
        "csrrci %5, mscratch, 1\n"
        "csrr %6, mscratch"
        : "=&r"(a), "=&r"(b), "=&r"(c), "=&r"(d), "=&r"(e), "=&r"(f), "=&r"(result)
        : "r"(0xa0u), "r"(15u), "r"(0x30u) : "memory");
    return a == 0 && b == 0xa0 && c == 0xaf && d == 0x8f && e == 3 && f == 0x13 && result == 0x12;
}

__attribute__((noinline)) static word mix(word value, word item) {
    const word rotated = (item << 5) | (item >> 27);
    const word comparison = item < 0x80000000u ? 3u : 7u;
    return (value ^ rotated) + ((item & 255u) ^ comparison);
}

void c_entry(void) {
    word value = 0;
    for (word i = 0; i < 4; ++i) {
        value = mix(value, inputs[i]);
    }
    byte_echo[1] = (unsigned char)value;
    half_echo[1] = (unsigned short)(value >> 8);
    value = (value & 0xff000000u) | (word)byte_echo[1] | ((word)half_echo[1] << 8);
    signed_echo[0] = (word)signed_byte;
    signed_echo[1] = (word)signed_half;
    if (signed_echo[0] != 0xffffff80u || signed_echo[1] != 0xffff8000u) value = 0;
    if (!timer_smoke()) value = 0;
    __asm__ volatile(".globl compiler_smoke_ecall\ncompiler_smoke_ecall:\n ecall" ::: "memory");
    if (*(volatile word*)0x70 != 11 || *(volatile word*)0x74 != (word)&compiler_smoke_ecall ||
        *(volatile word*)0x78 != 0) value = 0;
    if (!csr_smoke()) value = 0;
    __asm__ volatile("fence iorw, iorw\nfence.i" ::: "memory");
    signature = value;
    *(volatile word*)0x60 = value;
    *(volatile word*)0x40001000 = 'C';
}
