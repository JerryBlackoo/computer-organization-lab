typedef unsigned u32;
__attribute__((section(".user.text"), noinline))
u32 paged_calculate(void) {
    volatile u32 *data = (volatile u32 *)0x4000c000;
    volatile u32 stack[4];
    stack[0] = data[0];
    stack[1] = stack[0] + 1;
    stack[2] = (stack[1] << 1) ^ 0x55;
    stack[3] = stack[2] + 7;
    data[0] = stack[1];
    data[1] = stack[3];
    return stack[3];
}
