/* ============================================================================
 * BoxOS User Program Linker Script
 * ============================================================================
 * Memory layout for user-space programs (CABIN MEMORY MODEL):
 *   0x0000-0x0FFF - NULL trap zone (unmapped)
 *   0x1000-0x1FFF - Notify Page (User→Kernel communication)
 *   0x2000-0x2FFF - Result Page (Kernel→User communication)
 *   0x3000+       - Code, data, heap, stack (ENTRY POINT)
 *   Stack grows down from 0x00007FFFFFFFE000
 * ============================================================================
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

/* User program base address - MUST match VMM_CABIN_CODE_START (0x3000) */
USER_BASE = 0x3000;

SECTIONS
{
    /* Start at user base address */
    . = USER_BASE;

    /* Code section */
    .text : ALIGN(4096)
    {
        /* Entry point MUST be first for flat binary loading */
        *boxlib_start.o(.text)     /* Entry point first */
        *(.text .text.*)
    }

    /* Read-only data */
    .rodata : ALIGN(4096)
    {
        *(.rodata .rodata.*)
    }

    /* Initialized data */
    .data : ALIGN(4096)
    {
        *(.data .data.*)
    }

    /* Uninitialized data */
    .bss : ALIGN(4096)
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    /* Program end marker */
    __program_end = .;

    /* Discard unnecessary sections */
    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
