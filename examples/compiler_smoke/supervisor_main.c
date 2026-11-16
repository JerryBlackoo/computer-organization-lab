typedef unsigned int u32;

__attribute__((section(".supervisor.text"), noinline))
u32 supervisor_service(u32 first, u32 second) {
    register u32 value __asm__("a0") = first + second;
    register u32 service __asm__("a7") = 0x100;
    u32 stack;
    __asm__ volatile ("mv %0, sp" : "=r"(stack));
    *(volatile u32 *)0x150 = stack;
    __asm__ volatile ("ecall" : "+r"(value) : "r"(service) : "memory");
    return (value << 1) ^ 0x5a;
}

__attribute__((section(".user.text"), noinline, noreturn))
void supervisor_user_main(void) {
    register u32 first __asm__("a0") = 21;
    register u32 second __asm__("a1") = 8;
    register u32 service __asm__("a7") = 1;
    __asm__ volatile ("ecall" : "+r"(first) : "r"(second), "r"(service) : "memory");
    volatile u32 *result = (volatile u32 *)0xc000;
    result[0] = first;
    // The U-mode machine CSR read must fault into S and resume after this word.
    __asm__ volatile (".word 0x305022f3" ::: "t0", "memory");
    u32 checksum = 0;
    for (u32 i = 0; i < 4; ++i) checksum = (checksum << 3) ^ (7 + i);
    result[1] = checksum;
    first = result[0] == 6 && result[1] == 0xc42 ? 0x53555052 : 0xbad00001;
    service = 2;
    __asm__ volatile (".globl supervisor_user_exit_ecall\nsupervisor_user_exit_ecall:\necall"
        : "+r"(first) : "r"(service) : "memory");
    for (;;) __asm__ volatile ("nop");
}
