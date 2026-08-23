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

    /* User-access fault fixup table — populated by inline asm in
     * src/kernel/core/memory/uaccess.c. Each entry is a pair of 8-byte
     * RIP values: (fault_rip, recovery_rip). On a kernel #PF whose RIP
     * matches an entry, the VMM PF handler jumps frame->rip to the
     * recovery_rip — letting copy_to/from_user return a "bytes not
     * copied" count instead of panicking. The table is sorted by
     * fault_rip at link time so the lookup can binary-search. */
    .uaccess_fixup ALIGN(8) : AT(ADDR(.uaccess_fixup) - KERNEL_VMA) {
        __uaccess_fixup_start = .;
        KEEP(*(.uaccess_fixup))
        __uaccess_fixup_end = .;
    } :rodata

    /* Nameplate — the kernel's own "address -> name" table, generated from
     * this very link and linked back in by a second pass. It sits here, after
     * .rodata and before .data, for the same reason the userspace one does:
     * pass two moves .data and .bss and not a single function, so the
     * addresses the table names are still the addresses the functions have.
     * `nameplate verify` proves that on every build rather than trusting it.
     *
     * SHF_ALLOC, so it is simply part of the image the loader already brings
     * in. A panic that had to read a file to name a frame would be useless
     * for every fault that happens inside the filesystem, or before it. */
    .nameplate ALIGN(8) : AT(ADDR(.nameplate) - KERNEL_VMA) {
        __nameplate_start = .;
        KEEP(*(.nameplate))
        __nameplate_end = .;
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
