#ifndef BOX_NAMEPLATE_H
#define BOX_NAMEPLATE_H

/* ============================================================================
 * Nameplate — ask an address what it is called.
 *
 * Every BoxOS image carries a table, generated at link time from its own
 * symbols, that maps a code address to the name of the function containing
 * it. This is the in-process face of it. The table's format, and the reasons
 * it lives inside the image rather than in a file or in the kernel, are in
 * src/include/nameplate_format.h.
 *
 * What it costs: a binary search over read-only memory the loader already
 * mapped. No syscall, no allocation, no lock, no filesystem. That is the
 * whole point — the addresses worth naming are the ones you collected while
 * something was going wrong, and a diagnostic that needs the rest of the
 * system to be healthy is a diagnostic that fails when you need it.
 *
 * What it does NOT do: demangle. C++ names come back exactly as the linker
 * wrote them (_ZN3box7currentIcE5writeEPKcm). There is no demangler in the
 * tree, and building one at image-build time was measured and rejected —
 * demangling the C++ test image's names grows them from 3.7 MB to 9.7 MB,
 * with single names reaching 5266 characters.
 *
 * Line numbers are not here either, and not because nobody got to them:
 * the images that would ask for them are the C++ ones, and those must ship
 * without .debug_* to fit the 16 MB TagFS image at all.
 * ========================================================================== */

#ifdef __cplusplus
extern "C" {
#endif

#include "box/defs.h"

typedef struct NameplateSite {
    const char *Name;    /* NUL-terminated, mangled, points into the image */
    uintptr_t   Start;   /* address of the function's first byte */
    uint64_t    Offset;  /* how far the queried address sits into it */
} NameplateSite;

/*
 * Name the function containing `address`.
 *
 * Returns 1 and fills *site on a hit; 0 when this image carries no table,
 * when the address is below everything the table describes, or when it falls
 * in a gap past the end of the nearest function — padding, or a stretch of
 * code no symbol claims. Reporting nothing beats reporting the neighbour: a
 * backtrace that confidently names the wrong function costs more than one
 * that admits it does not know.
 */
int nameplate_lookup(uintptr_t address, NameplateSite *site);

/* How many names this image carries. 0 means it was linked without a table
 * (or with a corrupt one, which is treated the same way and never trusted). */
uint32_t nameplate_count(void);

#ifdef __cplusplus
}
#endif

#endif /* BOX_NAMEPLATE_H */
