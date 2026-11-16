ENTRY(_start)
PHDRS { machine PT_LOAD FLAGS(5); supervisor PT_LOAD FLAGS(5); user PT_LOAD FLAGS(5); }
SECTIONS {
    . = 0x8000;
    .machine : { *(.text.start) *(.text.machine) *(.text) *(.rodata*) } :machine
    ASSERT(. <= 0x9000, "machine firmware overlaps supervisor")
    . = 0x9000;
    .supervisor : { *(.supervisor.text) } :supervisor
    ASSERT(. <= 0xb000, "supervisor code overlaps user")
    . = 0xb000;
    .user : { *(.user.text) } :user
    ASSERT(. <= 0xc000, "user code overlaps data at 0xc000")
    .unexpected : { *(.data*) *(.sdata*) *(.bss*) *(.sbss*) *(COMMON) } :user
    ASSERT(SIZEOF(.unexpected) == 0, "smoke firmware uses explicit RAM addresses, not static data")
}
