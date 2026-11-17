typedef unsigned int u32;
volatile int signed_a = -13, signed_b = 3;
volatile u32 unsigned_a = 0xfffffffeu, unsigned_b = 3;

void c_entry(void) {
    volatile u32* const output = (volatile u32*)0x8000;
    output[0] = (u32)(signed_a / signed_b);
    output[1] = (u32)(signed_a % signed_b);
    output[2] = (unsigned_a / unsigned_b) ^ (unsigned_a % unsigned_b) ^
        (unsigned_a * unsigned_b) ^ (u32)(((unsigned long long)unsigned_a * unsigned_b) >> 32);
}
