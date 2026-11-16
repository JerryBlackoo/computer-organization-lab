ENTRY(_start)
PHDRS { text PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
    . = 0x8000;
    .text : { *(.text.start) *(.text*) *(.rodata*) } :text
    . = ALIGN(16);
    .data : { *(.data*) *(.sdata*) } :data
    .bss (NOLOAD) : { *(.bss*) *(.sbss*) *(COMMON) } :data
    ASSERT(. <= 0xf000, "program must leave room below the 0x10000 stack top")
}
