OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

/* MUST match VMM_CABIN_CODE_START (0x3000) */
USER_BASE = 0x3000;

SECTIONS
{
    . = USER_BASE;

    .text : ALIGN(4096)
    {
        *boxlib_start.o(.text)     /* entry point MUST be first for flat binary loading */
        *(.text .text.*)
    }

    .rodata : ALIGN(4096)
    {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(4096)
    {
        *(.data .data.*)
    }

    .bss : ALIGN(4096)
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    __program_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
