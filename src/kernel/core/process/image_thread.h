#ifndef IMAGE_THREAD_H
#define IMAGE_THREAD_H

#include "ktypes.h"
#include "error.h"
#include "vmm.h"

/*
 * A program's thread-local storage, made by the kernel for every strand of it
 * from the one template its image carries — a box's `thread` part, an ELF's
 * PT_TLS. x86-64 lays it out as variant II: the block lies right below the
 * thread pointer, the template's bytes at its start and zeros after them up to
 * the thread pointer, where the TCB begins with its own address (fs:0).
 *
 *     [ init | zeros .................. ][ self | 0 ]
 *     ^ tp - below                        ^ tp: the FS base, aligned to Align
 *
 * below is AlignUp(TotalBytes, Align): the distance the linker measured every
 * variable's offset from. A block anywhere else has each variable read beside
 * its bytes.
 *
 * The main strand's block is placed by the loader after the image, in the
 * room a program's image has. A spawned strand's is the page right below its
 * StrandInfo, which is its TCB (strand_rings.c): the cabin keeps a copy of the
 * template for them, and a strand of a program whose block does not fit that
 * page is not spawned.
 */
typedef struct {
    uint64_t       Align;        /* a power of two, at most a page */
    uint64_t       InitBytes;
    uint64_t       TotalBytes;   /* 0: the program keeps nothing per strand */
    const uint8_t *Init;
} ImageThread;

/* The bytes of the block below the thread pointer. */
uint64_t ImageThreadBelow(const ImageThread *thread);

/* The pages the main strand's block takes, its TCB included; 0 for none. */
uint64_t ImageThreadPages(const ImageThread *thread);

/* The main strand's block on the pages from `at` (page-aligned) in `vmm`,
 * writable and never executable; *tp the thread pointer. */
error_t ImageThreadPlace(vmm_context_t *vmm, const ImageThread *thread, uintptr_t at,
                         const char *name, uintptr_t *tp);

/* Whether a spawned strand's block fits the one page below its TCB. */
bool ImageThreadFitsStrand(const ImageThread *thread);

/* `thread` made the cabin's own: its template copied to the kernel's heap when
 * a strand's block can hold it, dropped when none can (the shape is kept, so
 * a spawn can say why it is refused). `name` is what a refusal calls the image. */
error_t ImageThreadKeep(const ImageThread *thread, const char *name, ImageThread *kept);

/* What ImageThreadKeep took, given back. */
void ImageThreadLetGo(ImageThread *kept);

/* A spawned strand's block poured into `page`, the zeroed page right below
 * its TCB, as the kernel sees it. */
void ImageThreadPour(const ImageThread *thread, uint8_t *page);

#endif /* IMAGE_THREAD_H */
