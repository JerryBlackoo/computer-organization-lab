.option norvc
.option norelax
.section .text.start
.globl _start
_start:
    lui sp, 0x10
    la t0, machine_trap_entry
    csrw mtvec, t0
    call c_entry
1:  j 1b

.p2align 2
machine_trap_entry:
    # The CPU saves PC/cause/value and interrupt state. Software preserves GPRs.
    addi sp, sp, -16
    sw t0, 0(sp)
    sw t1, 4(sp)
    sw t2, 8(sp)
    csrr t0, mcause
    csrr t1, mepc
    csrr t2, mtval
    sw t0, 0x70(zero)
    sw t1, 0x74(zero)
    sw t2, 0x78(zero)
    li t2, 0x80000007
    beq t0, t2, machine_timer_interrupt
    addi t1, t1, 4
    csrw mepc, t1
    j machine_trap_restore
machine_timer_interrupt:
    sw t1, 0x84(zero)
    li t0, 0x02004000
    li t1, -1
    sw t1, 4(t0)
    sw t1, 0(t0)
    lw t0, 0x80(zero)
    addi t0, t0, 1
    sw t0, 0x80(zero)
machine_trap_restore:
    lw t0, 0(sp)
    lw t1, 4(sp)
    lw t2, 8(sp)
    addi sp, sp, 16
    mret
