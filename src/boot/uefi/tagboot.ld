/*
 * tagboot.ld — Linker script for TagBoot UEFI application (PE32+ x86_64)
 *
 * The ELF shared object is converted to PE32+ by tools/make_efi.py.
 * Starting at VMA 0x1000 ensures PE sections begin at RVA >= 0x1000
 * (UEFI PE32+ rejects sections at RVA 0 because PE headers live there).
 * With ImageBase = 0, ELF VMAs equal PE RVAs, so the Python converter
 * can copy data verbatim and reuse ELF .rela.dyn entries as PE .reloc.
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(TagBootMain)

SECTIONS
{
    . = 0x1000;

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
        *(.data.rel.ro)
        *(.data.rel.ro.*)
    }

    .bss ALIGN(4096) : {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        __bss_end = .;
    }

    /*
     * Keep .rela.dyn — the Python converter reads it to produce the PE .reloc
     * section.  Without .reloc, UEFI cannot load the image at an address other
     * than ImageBase (0), which would cause an EFI_LOAD_ERROR on real hardware.
     */

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
        *(.debug*)
    }
}
