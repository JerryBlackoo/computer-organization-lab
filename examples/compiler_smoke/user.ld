ENTRY(_start)
PHDRS {
    machine_text PT_LOAD FLAGS(5);
    machine_data PT_LOAD FLAGS(6);
    user_text PT_LOAD FLAGS(5);
    user_data PT_LOAD FLAGS(6);
}
SECTIONS {
    . = 0x8000;
    .text : { *(.text.start) *(.text*) *(.rodata*) } :machine_text
    ASSERT(. <= 0x8800, "machine handler overlaps protected data")
    . = 0x8800;
    .machine_data : { *(.machine.data) *(.data*) *(.sdata*) } :machine_data
    .machine_bss (NOLOAD) : { *(.bss*) *(.sbss*) *(COMMON) } :machine_data
    ASSERT(. <= 0x9000, "machine data overlaps user text")
    . = 0x9000;
    .user_text : { *(.user.text) } :user_text
    ASSERT(. <= 0xa000, "user text exceeds PMP region")
    . = 0xa000;
    .user_data : { *(.user.data) } :user_data
    ASSERT(. <= 0xa800, "user data must leave stack space below 0xb000")
}
