ENTRY(_start)
PHDRS { machine PT_LOAD FLAGS(5); supervisor PT_LOAD FLAGS(5); user PT_LOAD FLAGS(5); }
SECTIONS {
 . = 0x8000; .machine : { *(.text.start) *(.text) *(.rodata*) } :machine
 ASSERT(. <= 0x9000, "M firmware exceeds one page")
 . = 0x9000; .supervisor : { *(.supervisor.text) } :supervisor
 ASSERT(. <= 0xa000, "S code exceeds one page")
 . = 0xa000; .user : { *(.user.text) } :user
 ASSERT(. <= 0xb000, "U code exceeds one page")
 .unexpected : { *(.data*) *(.sdata*) *(.bss*) *(.sbss*) *(COMMON) } :user
 ASSERT(SIZEOF(.unexpected) == 0, "firmware uses explicit RAM addresses")
}
