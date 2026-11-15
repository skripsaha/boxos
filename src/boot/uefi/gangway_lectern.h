#ifndef GANGWAY_LECTERN_H
#define GANGWAY_LECTERN_H

/*
 * The Lectern — the UEFI loader's reader of a volume's book — and
 * the Succession of kernels it is asked for: which kernel the loader jumps
 * into, each one tried held to the digests the book keeps of its blocks
 * before the jump.
 *
 * The book is read as the kernel's mount stands on it (§1.4, §4.5): every slot
 * of the Spine judged by the one rule every reader of a slot keeps
 * (volume_spine.h), those that hold tried newest first, the first whose
 * authoritative roots all read stood on. The kernel the Deed's BOOT names is
 * tried first, its blocks held to the DIGESTS tree of that slot's Tally with
 * no walk of FILES (§9.2 п.2). When it is not attested, the kernels the book
 * names by role (§16.7 п.3): the stitches past the slot's `at`, sewn on by
 * rules (а)/(б)/(б′)/(в), the records of the files kept in a role — the role
 * is bits of a file's flags, never a tag — replayed narrowly, each held to its
 * base, an Undone passing over what it names (§16.7 п.5); and every file of
 * FILES kept in one, role by role — kernel, boot, system — the newest gen
 * first. Each kernel is
 * held to its header (KERNEL at byte 2), placed where
 * it is jumped into, and its every block held to its digest — the record of
 * the ribbon that put it in, or DIGESTS: the first attested is jumped into;
 * one found broken is named and passed; one the book cannot vouch for is put
 * off (F2) — BOOT, when nothing shows it broken, is loaded not attested when
 * nothing else is attested, then the first put off; nothing of either, and the
 * loader stops with every kernel it tried and why.
 *
 * NOTHING OF UEFI HERE. The medium, memory, the console and the place a kernel
 * is jumped into are the caller's, handed in a desk: Gangway reads by Block IO,
 * gives pages and places at its load address (gangway_attest.c); the host
 * stand reads an image file and gives the heap (tools/gangway_stand.c) — one
 * text, so what the stand is held to against volume-read's oracle
 * (tools/volumeread/bootread.py, bootbook.py) is what Gangway does. The loops
 * that walk the book are the loader's own: the kernel's mount reads on a heap
 * and holds every file in memory, and a loader needs a few nodes and the
 * files kept in a role (§16.7 п.2, F1); the decisions — a slot's shape,
 * the order of slots, the trees that must read, whether a node holds, the
 * element that names a block, every record's codec — are the shared
 * headers', as the kernel takes them.
 *
 * Integer types come from the includer, as volume_deed.h says.
 */

#include "volume_spine.h"
#include "volume_ribbon.h"
#include "volume_digest.h"

/* The most a kernel may be that this loader places. */
#define KERNEL_MAX_SIZE       0x2000000ull   /* 32 MB */

/*
 * What the Lectern is handed: how to read the volume — `sectors` 512-byte
 * sectors from `vlba`, counted from the ground's first, into `into`; 1 read,
 * 0 the medium would not — room for what it reads, given back when it is done
 * (take answers NULL for none), a line said, with no prefix and no end of
 * line, and the room a kernel of `bytes` is placed in where it is jumped into
 * (place answers NULL when it cannot be had, said by the desk), given back
 * when the kernel there is not the one. `uuid` seeds every sector's sum and
 * every block's digest; `frame` is the Deed's, already held to
 * VolumeFrameCheck; `block_end` the block where the far copy of the Deed sits
 * — nothing at or past it is read; `atom_blocks` the volume's atom (its
 * GEOMETRY, BookAtomOf) in blocks — a Quire of the ribbon begins on one.
 */
typedef struct {
    void          *ctx;
    int          (*read)(void *ctx, uint64_t vlba, uint32_t sectors, uint8_t *into);
    uint8_t     *(*take)(void *ctx, uint64_t bytes);
    void         (*give)(void *ctx, uint8_t *room, uint64_t bytes);
    void         (*say)(void *ctx, const char *line);
    uint8_t     *(*place)(void *ctx, uint64_t bytes);
    void         (*unplace)(void *ctx, uint8_t *room, uint64_t bytes);
    const uint8_t *uuid;
    VolumeFrame    frame;
    uint64_t       block_end;
    uint64_t       atom_blocks;
} LecternDesk;

/* What a kernel's blocks are found to be (§16.7 п.4, F2). */
typedef enum {
    ATTEST_HELD,        /* every block matches the digest the book keeps of it */
    ATTEST_BROKEN,      /* a block does not: the loader does not jump into it */
    ATTEST_UNCHECKED,   /* nothing to hold some block to, and none found broken */
} AttestSays;

#define LECTERN_LINE_BYTES 256u

/*
 * The kernel the Succession chose: HELD, attested and placed; UNCHECKED,
 * placed, nothing to hold it to and nothing showing it broken (F2); BROKEN,
 * none may be jumped into, `line` the sentence the loader stops with.
 * Placed: `image` (the desk's room, `blocks` blocks), its size in `bytes`,
 * and where it lay — the volume block of its first block (0 for a hole) and
 * the runs it was read from.
 */
typedef struct {
    AttestSays says;
    uint8_t   *image;
    uint64_t   bytes;
    uint64_t   blocks;
    uint64_t   first;
    uint64_t   runs;
    char       line[LECTERN_LINE_BYTES];
} LecternKernel;

/*
 * The kernel the loader jumps into on the volume the desk reads: BOOT's first
 * (`boot` NULL when the Deed names none), then the kernels the book names by
 * role — every line said as it is read and every kernel's verdict as it is
 * found, the choice last.
 */
void LecternSuccession(const LecternDesk *desk, const VolumeBoot *boot, LecternKernel *kernel);

#endif /* GANGWAY_LECTERN_H */
