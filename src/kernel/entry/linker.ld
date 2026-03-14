ENTRY(_start)

KERNEL_VMA = 0xFFFFFFFF80000000;
KERNEL_LMA = 0x100000;

PHDRS
{
    text PT_LOAD FLAGS(5);     /* R+X */
    rodata PT_LOAD FLAGS(4);   /* R */
    data PT_LOAD FLAGS(6);     /* R+W */
}

SECTIONS
{
    . = KERNEL_VMA + KERNEL_LMA;
    _kernel_start = .;
    _kernel_phys_start = . - KERNEL_VMA;
    _text_start = .;

    .text ALIGN(512) : AT(ADDR(.text) - KERNEL_VMA) {
        *(.text)
        *(.text.*)
        *(.tagfs)
    } :text

    _text_end = ALIGN(512);

    .rodata ALIGN(512) : AT(ADDR(.rodata) - KERNEL_VMA) {
        *(.rodata)
        *(.rodata.*)
    } :rodata

    _rodata_end = ALIGN(512);

    .data ALIGN(512) : AT(ADDR(.data) - KERNEL_VMA) {
        *(.data)
        *(.data.*)
    } :data

    .bss ALIGN(512) : AT(ADDR(.bss) - KERNEL_VMA) {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        __bss_end = .;
    } :data

    _kernel_end = ALIGN(512);
    _kernel_phys_end = _kernel_end - KERNEL_VMA;

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame)
    }
}
