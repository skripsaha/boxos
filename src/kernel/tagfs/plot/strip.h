#ifndef TAGFS_STRIP_H
#define TAGFS_STRIP_H

#include "../../../lib/kernel/ktypes.h"

/*
 * Strip — a run of a file's ground as memory holds it: a strip of land inside
 * the file's Plot.
 *
 * `count` blocks from block `start`, both as wide as memory counts.
 *
 * `kind` says what the blocks are. STRIP_DATA is ground holding the file's
 * bytes, the file its one owner; STRIP_HOLE is blocks of the file that read as
 * zeros and own no ground; STRIP_SHARED is ground holding the file's bytes
 * that other files may name too (a run that carries SHARED) —
 * read as DATA is, never written in place, let go by the last of its owners
 * (ink/commonhold.h); the book's records say all three (§2.8).
 * `birth` is the stitch that laid the run.
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
