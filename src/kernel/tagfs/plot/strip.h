#ifndef TAGFS_STRIP_H
#define TAGFS_STRIP_H

#include "../../../lib/kernel/ktypes.h"

/*
 * Strip — a run of a file's ground as memory holds it, whatever format the
 * volume's book is written in: a strip of land inside the file's Plot.
 *
 * `count` blocks from block `start`, both as wide as memory counts — a Strip is
 * never cut to the width a format gives a run on the medium. Format v1's
 * extent, a u32 first block and a u16 count, is the packer's alone
 * (plot_pack.c): PackV1 cuts a Strip longer than an extent says into as many
 * extents as it takes and refuses one that names a block past a u32, and
 * UnpackV1 reads neighbouring extents back as one Strip.
 *
 * `kind` says what the blocks are. STRIP_DATA is ground holding the file's
 * bytes, the file its one owner; STRIP_HOLE is blocks of the file that read as
 * zeros and own no ground; STRIP_SHARED is ground holding the file's bytes
 * that other files may name too (spec_v2 §8.2, a run that carries SHARED) —
 * read as DATA is, never written in place, let go by the last of its owners
 * (ink/commonhold.h). Format v2's records can say the two last (§2.8) and
 * format v1's cannot — so every Strip of a v1 volume is STRIP_DATA, and PackV1
 * refuses any other.
 * `birth` is the stitch that laid the run on a v2 volume; format v1 has no such
 * number, and its Strips carry 0.
 */
typedef struct {
    uint64_t start;
    uint32_t count;
    uint32_t kind;
    uint64_t birth;
} Strip;

STATIC_ASSERT(sizeof(Strip) == 24, "Strip must be 24 bytes");

#define STRIP_DATA    0u
#define STRIP_HOLE    1u
#define STRIP_SHARED  2u

/* Whether a Strip of `kind` lies on ground the file reads its bytes from —
 * DATA or SHARED — and is no hole. */
static inline bool StripOnGround(uint32_t kind)
{
    return kind == STRIP_DATA || kind == STRIP_SHARED;
}

#endif /* TAGFS_STRIP_H */
