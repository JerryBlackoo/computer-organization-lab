typedef unsigned int word;
__attribute__((section(".machine.data"))) volatile word kernel_guard = 0xcafebabe;
__attribute__((section(".user.data"))) volatile word user_buffer[4] = {1, 7, 31, 255};

__attribute__((section(".user.text"))) void user_entry(void) {
    volatile word locals[4];
    word value = 0;
    for (word i = 0; i < 4; ++i) {
        locals[i] = user_buffer[i];
        value = (value << 3) ^ locals[i];
    }
    user_buffer[1] = value;
    __asm__ volatile("fence rw, rw\nfence.i" ::: "memory");
    register word result __asm__("a0") = value;
    register word service __asm__("a7") = 1;
    __asm__ volatile("ecall" : "+r"(result) : "r"(service) : "memory");
    const word reply = result;
    __asm__ volatile(".globl denied_user_csr\ndenied_user_csr:\ncsrr t0, mtvec" ::: "t0", "memory");
    __asm__ volatile(".globl denied_user_mmio\ndenied_user_mmio:\nlw t0, 0(%0)"
        : : "r"((word*)0x40001000) : "t0", "memory");
    __asm__ volatile(".globl denied_user_store\ndenied_user_store:\nsw zero, 0(%0)"
        : : "r"((word*)0x8800) : "memory");
    __asm__ volatile("jalr zero, 0(%0)\n.globl user_after_bad_jump\nuser_after_bad_jump:"
        : : "r"((word*)0x8000) : "memory");
    result = reply == 0x55 && user_buffer[1] == value && locals[3] == 255 ? 0x50524f54 : 0xbad;
    service = 2;
    __asm__ volatile("ecall" : "+r"(result) : "r"(service) : "memory");
    for (;;) __asm__ volatile("j .");
}
