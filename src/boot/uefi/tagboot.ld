/*
 * tagboot.ld — Linker script for TagBoot UEFI application (PE32+ x86_64)
 *
 * Produces a relocatable shared object that objcopy converts to a PE32+ EFI
 * binary.  The entry point symbol must be TagBootMain.
 *
 * EFI applications are loaded at an arbitrary virtual address by the firmware;
 * -fpic + this script make the binary fully position-independent.
 *
 * Section names match what objcopy --target=efi-app expects:
 *   .text    → .text   (code)
 *   .data    → .data   (read-write data)
 *   .rodata  → .rodata (read-only data, merged into .text by objcopy)
 *   .bss     → .bss    (zero-initialised, not present in file)
 *
 * The output format is elf64-x86-64; objcopy converts it to pei-x86-64.
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(TagBootMain)

SECTIONS
{
    . = 0;

    .text ALIGN(4096) : {
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata.*)
    }

    .data ALIGN(4096) : {
        *(.data)
        *(.data.*)
        *(.got)
        *(.got.plt)
    }

    .bss ALIGN(4096) : {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        __bss_end = .;
    }

    /DISCARD/ : {
        *(.note*)
        *(.comment*)
        *(.eh_frame*)
        *(.gcc_except_table*)
        *(.dynamic*)
        *(.dynsym*)
        *(.dynstr*)
        *(.hash*)
        *(.gnu.hash*)
        *(.plt*)
        *(.rela.plt*)
        *(.rel.plt*)
    }
}
