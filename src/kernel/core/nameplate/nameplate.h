#ifndef KERNEL_NAMEPLATE_H
#define KERNEL_NAMEPLATE_H

/*
 * Nameplate, kernel side — name a user-mode address from the table its own
 * image carries.
 *
 * The format and the reasoning behind it are in src/include/nameplate_format.h.
 * Userspace reads its own table directly (boxlib box/nameplate.h); the kernel
 * has to reach into another address space to do the same, which is why this
 * half exists at all. It exists for one caller: the user-mode fault dump,
 * which walked the frame chain and could only print addresses.
 *
 * Two entry points, at opposite ends of a process's life:
 *
 *   nameplate_locate()   at load, with the whole image in kernel memory —
 *                        finds the table and reports where it will live once
 *                        the image is mapped.
 *   nameplate_name_at()  at a fault, with nothing to go on but the address
 *                        and the process — reads the table through the SMAP
 *                        window with page-fault fixup, so a process that
 *                        corrupted its own table cannot take the kernel down
 *                        with it while being diagnosed.
 */

#include "ktypes.h"

/* A resolved name, sized for a dump line rather than for the longest name a
 * C++ mangling can produce. This runs inside the exception handler: there is
 * no allocating here, and a truncated name that reaches the operator beats a
 * complete one that never gets printed. Truncation is marked, not hidden. */
#define NAMEPLATE_NAME_MAX 96

typedef struct NameplateName {
    char     Text[NAMEPLATE_NAME_MAX];
    uint64_t Offset;   /* how far the queried address sits into the function */
} NameplateName;

/*
 * Find the Nameplate in a program image held in KERNEL memory, and report the
 * user virtual address it will occupy once mapped.
 *
 * `image`/`bytes` is the whole file as loaded; `load_base` is the address the
 * image is mapped at, used only for flat binaries.
 *
 * Both shapes BoxOS loads are handled, because both are shipped: an ELF is
 * asked its section headers, which is exact; a flat binary has no section
 * headers at all — the shell is one — so its image is scanned for the table's
 * magic and every candidate is put through the format's own validator before
 * it is believed. A false positive would have to be thirty-two bytes of
 * self-consistent header, and the worst it could do is put a wrong name in a
 * diagnostic.
 *
 * Returns 1 and fills both out-parameters on success, 0 when the image
 * carries no usable table.
 */
int nameplate_locate(const void *image, uint64_t bytes, uintptr_t load_base,
                     uintptr_t *out_va, uint64_t *out_bytes);

/*
 * Name the function containing `addr` in a process whose address space is the
 * one currently installed. `table_va`/`table_bytes` come from the process
 * record, where nameplate_locate() put them.
 *
 * Returns 1 and fills *out on a hit; 0 when the process has no table, the
 * table does not survive validation, the address is outside it, or it lands
 * past the end of the nearest function. Every read goes through get_user_u32,
 * so a wild or unmapped table ends the lookup instead of faulting the kernel
 * inside its own exception handler.
 */
/* The kernel's own table, resident in the kernel image. Same search, read
 * with plain loads instead of get_user — see nameplate.c. */
int nameplate_name_at_kernel(uintptr_t table_va, uint64_t table_bytes,
                             uintptr_t addr, NameplateName *out);

int nameplate_name_at(uintptr_t table_va, uint64_t table_bytes, uintptr_t addr,
                      NameplateName *out);

#endif /* KERNEL_NAMEPLATE_H */
