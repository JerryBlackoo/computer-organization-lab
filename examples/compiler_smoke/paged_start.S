.option norvc
.option norelax
.section .text.start,"ax",@progbits
.globl _start
_start:
    li sp, 0x10000
    la t0, machine_fail
    csrw mtvec, t0
    # All tables are built by the simulated CPU in RAM, not by the host.
    li t0, 0x100
    li t1, 0x200
1:  sw zero, 0(t0)
    addi t0, t0, 4
    bne t0, t1, 1b
    li t0, 0x1000
    li t1, 0x3000
1:  sw zero, 0(t0)
    addi t0, t0, 4
    bne t0, t1, 1b
    li t0, 0xfeedface
    sw t0, 0x180(zero)
    li t0, 41
    li t1, 0xc000
    sw t0, 0(t1)
    li t0, 71
    li t1, 0x5000
    sw t0, 0(t1)
    li t0, 99
    li t1, 0x3000
    sw t0, 0(t1)
    li t1, 0x1000
    li t0, 0xcf                 # identity 4MiB supervisor superpage, RWXAD
    sw t0, 0(t1)
    li t0, 0x801                # root[256] -> table at 0x2000
    sw t0, 1024(t1)
    li t1, 0x2000
    li t0, 0x100004c7           # S-only UART MMIO VA/PA 0x40001000
    sw t0, 4(t1)
    li t0, 0x1417               # U RW page, A=D=0, physical 0x5000
    sw t0, 20(t1)
    li t0, 0x285b               # U RX code at physical 0xa000, A=1
    sw t0, 40(t1)
    li t0, 0x30d7               # U RWAD data at physical 0xc000
    sw t0, 48(t1)
    li t0, 0x3059               # U execute-only alias of data, A=1
    sw t0, 52(t1)
    li t0, 0x38d7               # U RWAD stack at physical 0xe000
    sw t0, 56(t1)
    li t0, 0x1fffffff
    csrw pmpaddr0, t0
    li t0, 0x1f
    csrw pmpcfg0, t0
    li t0, 0xb100               # U ECALL + instruction/load/store page faults
    csrw medeleg, t0
    la t0, supervisor_trap
    csrw stvec, t0
    li t0, 0x80000001
    csrw satp, t0
    sfence.vma zero, zero
    la t0, supervisor_entry
    csrw mepc, t0
    li t0, 0x800
    csrw mstatus, t0
    mret
machine_fail:
    csrr t0, mcause
    sw t0, 0x17c(zero)
    li t0, 0xbad00001
    sw t0, 0x100(zero)
1:  j 1b

.section .supervisor.text,"ax",@progbits
.p2align 2
supervisor_entry:
    li sp, 0xe000
    li t0, 0x4000c000
.globl sum_fault
sum_fault:
    lw t1, 0(t0)                # S cannot read U page without SUM
    li t1, 0x40000
    csrs sstatus, t1
    lw a3, 0(t0)                # succeeds with SUM; 41
    sw a3, 0x108(zero)
    li t0, 0x4000d000
.globl mxr_fault
mxr_fault:
    lw t1, 0(t0)                # X-only page still unreadable
    li t1, 0x80000
    csrs sstatus, t1
    lw a3, 0(t0)                # succeeds with MXR; 41
    sw a3, 0x10c(zero)
    li t0, 0xc0000
    csrc sstatus, t0
    la t0, user_entry
    li t1, 0x40000000
    add t0, t0, t1
    csrw sepc, t0
    li sp, 0x4000f000
    sret

# No stack or C calls in this tiny handler: t3..t6 are explicitly guest-reserved.
.p2align 2
supervisor_trap:
    csrr t3, scause
    lw t4, 0x104(zero)
    slli t5, t4, 3
    slli t6, t4, 2
    add t5, t5, t6
    addi t5, t5, 0x200
    sw t3, 0(t5)
    csrr t6, stval
    sw t6, 4(t5)
    csrr t6, sepc
    sw t6, 8(t5)
    addi t4, t4, 1
    sw t4, 0x104(zero)
    li t4, 8
    beq t3, t4, supervisor_done
    li t4, 12
    beq t3, t4, fetch_recover
    csrr t6, stval
    li t4, 0x40005000
    bne t6, t4, skip_fault
    # Svade software A/D repair and retry the same instruction.
    li t4, 0x2014
    lw t5, 0(t4)
    li t6, 15
    beq t3, t6, dirty_repair
    ori t5, t5, 64
    j repair_commit
dirty_repair:
    ori t5, t5, 192
repair_commit:
    sw t5, 0(t4)
    sfence.vma zero, zero
    sret
skip_fault:
    csrr t6, sepc
    addi t6, t6, 4
    csrw sepc, t6
    sret
fetch_recover:
    csrw sepc, s11
    sret
supervisor_done:
    li sp, 0xe000
    sw a0, 0x110(zero)
    li t0, 0x40000
    csrs sstatus, t0
    li t0, 0x4000c000
    lw t1, 0(t0)                # cache old data translation
    sw t1, 0x114(zero)
    li t1, 0xcd7                # remap VA data page to physical 0x3000
    li t2, 0x2030
    sw t1, 0(t2)
    lw t1, 0(t0)                # old TLB mapping until SFENCE
    sw t1, 0x118(zero)
    sfence.vma t0, zero
    lw t1, 0(t0)                # now reads 99 from the new frame
    sw t1, 0x11c(zero)
    li t0, 0x53563332
    sw t0, 0x100(zero)
    li t0, 0x40001000
    li t1, 86
    sw t1, 0(t0)                # UART 'V'
    li t1, 33
    sw t1, 0(t0)                # UART '!'
1:  wfi
    j 1b

.section .user.text,"ax",@progbits
.p2align 2
user_entry:
    call paged_calculate
    li t0, 0x180
    li a2, 55
.globl user_load_fault
user_load_fault:
    lw a2, 0(t0)                # cannot read supervisor secret
    li t1, 0x4000c000
    sw a2, 8(t1)                # faulting load must not alter a2
.globl user_store_fault
user_store_fault:
    sw zero, 0(t0)              # cannot corrupt supervisor secret
    li t0, 0x4000a000
.globl user_code_store_fault
user_code_store_fault:
    sw zero, 0(t0)              # RX user text is not writable
    li t0, 0x40007000
.globl user_unmapped_fault
user_unmapped_fault:
    lw t1, 0(t0)
    li t0, 0x40005000
.globl user_accessed_fault
user_accessed_fault:
    lw t1, 0(t0)                # A=0 -> repair -> retry -> 71
    li t2, 0x4000c000
    sw t1, 12(t2)
    li t1, 99
.globl user_dirty_fault
user_dirty_fault:
    sw t1, 0(t0)                # D=0 -> repair -> retry -> 99
    la s11, user_fetch_after
    li t0, 0x9000
    jalr ra, t0, 0              # U cannot execute supervisor page
user_fetch_after:
.globl user_exit_ecall
user_exit_ecall:
    ecall
1:  j 1b
