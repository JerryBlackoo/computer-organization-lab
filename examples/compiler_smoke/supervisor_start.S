.option norvc
.option norelax
.section .text.start,"ax",@progbits
.globl _start
_start:
    li sp, 0x10000
    csrw mscratch, sp
    # Reset instrumentation on warm reset as well as cold power-on.
    li t0, 0x100
    li t1, 0x154
1:  sw zero, 0(t0)
    addi t0, t0, 4
    bne t0, t1, 1b
    la t0, machine_trap
    csrw mtvec, t0
    la t0, supervisor_timer_trap
    csrw stvec, t0
    # Bare mode: S and U share PMP policy; page isolation is a later Sv32 test.
    li t0, 0x1fffffff
    csrw pmpaddr0, t0
    li t0, 0x1f
    csrw pmpcfg0, t0
    li t0, 0x104
    csrw medeleg, t0             # U ECALL and illegal instruction -> S.
    li t0, 0x20
    csrw mideleg, t0             # STIP -> S; machine timer stays in M.
    li t0, 0xa0
    csrw mie, t0                 # MTIE + STIE.
    la t0, supervisor_entry
    csrw mepc, t0
    li t0, 0x800
    csrw mstatus, t0             # MPP=S.
    li sp, 0xf000
    mret

.section .text.machine,"ax",@progbits
.p2align 2
machine_trap:
    csrrw sp, mscratch, sp
    addi sp, sp, -32
    sw t0, 0(sp)
    sw t1, 4(sp)
    sw t2, 8(sp)
    sw t3, 12(sp)
    sw a0, 16(sp)
    csrr t0, mcause
    li t1, 0x80000007
    beq t0, t1, machine_timer
    li t1, 9
    bne t0, t1, firmware_failure
    lw t0, 0x110(zero)
    addi t0, t0, 1
    sw t0, 0x110(zero)
    csrr t0, mstatus
    sw t0, 0x128(zero)
    csrr t0, mepc
    sw t0, 0x12c(zero)
    addi t0, t0, 4
    csrw mepc, t0
    li t0, 0x100
    beq a7, t0, machine_compute
    li t0, 0x101
    beq a7, t0, machine_arm_timer
    li t0, 0x102
    bne a7, t0, firmware_failure
    li t0, 0x20
    csrc mip, t0                 # Firmware acknowledges STIP after S handles it.
    j machine_restore
machine_compute:
    addi a0, a0, 17
    sw a0, 16(sp)
    j machine_restore
machine_arm_timer:
    li t0, 0x0200bff8
1:  lw t2, 4(t0)
    lw t1, 0(t0)
    lw t3, 4(t0)
    bne t2, t3, 1b
    addi t3, t1, 1024
    sltu t1, t3, t1
    add t2, t2, t1
    li t0, 0x02004000
    li t1, -1
    sw t1, 0(t0)
    sw t2, 4(t0)
    sw t3, 0(t0)
    j machine_restore
machine_timer:
    li t0, 0x02004000
    li t1, -1
    sw t1, 0(t0)
    sw t1, 4(t0)
    li t0, 0x20
    csrs mip, t0                 # Forward the expired machine timer as STIP.
    lw t0, 0x10c(zero)
    addi t0, t0, 1
    sw t0, 0x10c(zero)
    csrr t0, mepc
    sw t0, 0x13c(zero)
machine_restore:
    lw t0, 0(sp)
    lw t1, 4(sp)
    lw t2, 8(sp)
    lw t3, 12(sp)
    lw a0, 16(sp)
    addi sp, sp, 32
    csrrw sp, mscratch, sp
    mret
firmware_failure:
    li t0, 0xbad00002
    sw t0, 0x100(zero)
1:  j 1b

.section .supervisor.text,"ax",@progbits
.p2align 2
supervisor_entry:
    csrsi sstatus, 2
    li a7, 0x101
    ecall
    wfi
.globl supervisor_wfi_after
supervisor_wfi_after:
    csrci sstatus, 2
    la t0, supervisor_user_trap
    csrw stvec, t0
    csrw sscratch, sp
    la t0, supervisor_user_entry
    csrw sepc, t0
    csrw sstatus, zero           # SPP=U.
    li sp, 0xd000
    sret
supervisor_timer_trap:
    addi sp, sp, -32
    sw t0, 0(sp)
    sw t1, 4(sp)
    sw a0, 8(sp)
    sw a7, 12(sp)
    csrr t0, scause
    li t1, 0x80000005
    bne t0, t1, supervisor_failure
    lw t0, 0x108(zero)
    addi t0, t0, 1
    sw t0, 0x108(zero)
    csrr t0, sepc
    sw t0, 0x140(zero)
    csrr t0, sstatus
    sw t0, 0x144(zero)
    li a7, 0x102
    ecall
    lw t0, 0(sp)
    lw t1, 4(sp)
    lw a0, 8(sp)
    lw a7, 12(sp)
    addi sp, sp, 32
    sret
supervisor_user_trap:
    csrrw sp, sscratch, sp
    addi sp, sp, -64
    sw ra, 0(sp)
    sw t0, 4(sp)
    sw t1, 8(sp)
    sw t2, 12(sp)
    sw a0, 16(sp)
    sw a1, 20(sp)
    sw a2, 24(sp)
    sw a3, 28(sp)
    sw a4, 32(sp)
    sw a5, 36(sp)
    sw a6, 40(sp)
    sw a7, 44(sp)
    sw t3, 48(sp)
    sw t4, 52(sp)
    sw t5, 56(sp)
    sw t6, 60(sp)
    csrr t0, scause
    sw t0, 0x118(zero)
    csrr t1, sepc
    sw t1, 0x11c(zero)
    csrr t2, sstatus
    sw t2, 0x120(zero)
    lw t2, 0x104(zero)
    addi t2, t2, 1
    sw t2, 0x104(zero)
    li t2, 2
    beq t0, t2, supervisor_illegal
    li t2, 8
    bne t0, t2, supervisor_failure
    li t2, 2
    beq a7, t2, supervisor_done
    li t2, 1
    bne a7, t2, supervisor_failure
    call supervisor_service
    sw a0, 16(sp)
    sw a0, 0x114(zero)
    j supervisor_return
supervisor_illegal:
    sw t1, 0x124(zero)
    csrr t0, stval
    sw t0, 0x138(zero)
    lw t0, 0x134(zero)
    addi t0, t0, 1
    sw t0, 0x134(zero)
supervisor_return:
    csrr t0, sepc
    addi t0, t0, 4
    csrw sepc, t0
    lw ra, 0(sp)
    lw t0, 4(sp)
    lw t1, 8(sp)
    lw t2, 12(sp)
    lw a0, 16(sp)
    lw a1, 20(sp)
    lw a2, 24(sp)
    lw a3, 28(sp)
    lw a4, 32(sp)
    lw a5, 36(sp)
    lw a6, 40(sp)
    lw a7, 44(sp)
    lw t3, 48(sp)
    lw t4, 52(sp)
    lw t5, 56(sp)
    lw t6, 60(sp)
    addi sp, sp, 64
    csrrw sp, sscratch, sp
    sret
supervisor_done:
    sw a0, 0x100(zero)
    csrr t0, sscratch
    sw t0, 0x14c(zero)
    li t0, 0x40001000
    li t1, 'S'
    sw t1, 0(t0)
    li t1, '!'
    sw t1, 0(t0)
1:  j 1b
supervisor_failure:
    li t0, 0xbad00003
    sw t0, 0x100(zero)
1:  j 1b

.section .user.text,"ax",@progbits
.p2align 2
supervisor_user_entry:
    call supervisor_user_main
1:  j 1b
