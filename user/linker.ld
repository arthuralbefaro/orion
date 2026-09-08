ENTRY(_start)

SECTIONS
{
    . = 0x400000;
    .text   : { *(.text .text.*) }
    . = ALIGN(0x1000);
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(0x1000);
    .data   : { *(.data .data.*) }
    .bss    : { *(.bss .bss.*) *(COMMON) }
    /DISCARD/ : { *(.eh_frame*) *(.note .note.*) *(.comment) }
}
