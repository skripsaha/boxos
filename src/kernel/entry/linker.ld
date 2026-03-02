ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);     /* R+X */
    rodata PT_LOAD FLAGS(4);   /* R */
    data PT_LOAD FLAGS(6);     /* R+W */
}

SECTIONS
{
    . = 0x100000;
    _kernel_start = .;
    _text_start = .;

    .text ALIGN(512) : {
        *(.text)
        *(.text.*)
        *(.tagfs)
    } :text

    _text_end = ALIGN(512);

    .rodata ALIGN(512) : {
        *(.rodata)
        *(.rodata.*)
    } :rodata

    _rodata_end = ALIGN(512);

    .data ALIGN(512) : {
        *(.data)
        *(.data.*)
    } :data

    .bss ALIGN(512) : {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        __bss_end = .;
    } :data

    _kernel_end = ALIGN(512);

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame)
    }
}
