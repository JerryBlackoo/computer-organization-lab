.option norvc
.option norelax
.section .text.start
.globl _start
_start:
    lui sp, 0xf
    call c_entry
    ebreak
