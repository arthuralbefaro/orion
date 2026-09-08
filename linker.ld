/*
 * linker.ld - layout do kernel na memória virtual (higher half).
 *
 * O Limine exige o kernel nos 2 GiB superiores do espaço de endereços;
 * 0xffffffff80000000 é o começo dessa região. Cada seção vai num PT_LOAD
 * separado para o bootloader aplicar as permissões certas (RX / R / RW).
 */
ENTRY(kmain)

PHDRS
{
    limine_requests PT_LOAD;
    text            PT_LOAD;
    rodata          PT_LOAD;
    data            PT_LOAD;
}

SECTIONS
{
    . = 0xffffffff80000000;
    __kernel_start = .;

    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :limine_requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __text_start = .;
    .text : {
        *(.text .text.*)
    } :text
    __text_end = .;

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __rodata_start = .;
    .rodata : {
        *(.rodata .rodata.*)
    } :rodata
    __rodata_end = .;

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __data_start = .;
    .data : {
        *(.data .data.*)
    } :data

    /* .bss por último no segmento de dados: não ocupa espaço no arquivo. */
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data
    __data_end = .;
    __kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
    }
}
