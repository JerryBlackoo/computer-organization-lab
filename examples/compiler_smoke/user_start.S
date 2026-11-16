.option norvc
.option norelax
.section .text.start
.globl _start
_start:
    li sp, 0x10000
    csrw mscratch, sp
    la t0, user_trap_entry
    csrw mtvec, t0
    # U RX code [0x9000,0xa000), U RW data/stack [0xa000,0xb000).
    li t0, 0x2400
    csrw pmpaddr0, t0
    li t0, 0x2800
    csrw pmpaddr1, t0
    li t0, 0x2c00
    csrw pmpaddr2, t0
    li t0, 0x000b0d00
    csrw pmpcfg0, t0
    # Warm a protected kernel line before entering U; PMP must still reject U.
    li t0, 0x8800
    lw t1, 0(t0)
    la t0, user_entry
    csrw mepc, t0
    csrw mstatus, zero
    li sp, 0xb000
    mret

.p2align 2
user_trap_entry:
    csrrw sp, mscratch, sp
    addi sp, sp, -32
    sw t0, 0(sp)
    sw t1, 4(sp)
    sw t2, 8(sp)
    sw t3, 12(sp)
    sw t4, 16(sp)
    sw t5, 20(sp)
    sw t6, 24(sp)
    sw ra, 28(sp)
    lw t0, 0x64(zero)
    slli t1, t0, 4
    addi t1, t1, 0x100
    csrr t2, mcause
    csrr t3, mepc
    csrr t4, mtval
    csrr t5, mstatus
    sw t2, 0(t1)
    sw t3, 4(t1)
    sw t4, 8(t1)
    sw t5, 12(t1)
    addi t0, t0, 1
    sw t0, 0x64(zero)
    li t0, 8
    bne t2, t0, user_trap_return
    li t0, 2
    beq a7, t0, user_test_done
    li t0, 0x40001000
    li t1, 'U'
    sw t1, 0(t0)
    li a0, 0x55
user_trap_return:
    li t0, 1
    bne t2, t0, 2f
    # A denied instruction fetch records the inaccessible target, not JALR.
    la t3, user_after_bad_jump
    j 3f
2:
    addi t3, t3, 4
3:
    csrw mepc, t3
    lw t0, 0(sp)
    lw t1, 4(sp)
    lw t2, 8(sp)
    lw t3, 12(sp)
    lw t4, 16(sp)
    lw t5, 20(sp)
    lw t6, 24(sp)
    lw ra, 28(sp)
    addi sp, sp, 32
    csrrw sp, mscratch, sp
    mret
user_test_done:
    sw a0, 0x60(zero)
    li t0, 0x40001000
    li t1, '!'
    sw t1, 0(t0)
1:  j 1b
