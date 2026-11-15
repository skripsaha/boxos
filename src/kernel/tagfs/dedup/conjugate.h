#ifndef TAGFS_DEDUP_CONJUGATE_H
#define TAGFS_DEDUP_CONJUGATE_H

#include "../tagfs.h"

/*
 * Conjugate — a duplicate laid on the blocks it duplicates: blocks of one file
 * of a volume that hold, byte for byte, what blocks of another hold are named
 * by a run SHARED on the other's ground, and its own rest in Fallow and go
 * back — the volume holds the bytes once. The file's bytes, size and times are
 * what they were; only where they lie changes.
 *
 * ‼ THE BYTES DECIDE, NOT THE DIGESTS (§16.7 п.8, box_hash.h). Each pair of
 * blocks is held to the digests the book holds for them first — two that
 * differ refuse it with no block read — and then read and compared byte for
 * byte: 64 bits are no proof against a collision, nor against a writer who
 * looks for one. The target's block is held to its digest too, so nothing is
 * laid on a block that rotted. The reading is done with no TagFS lock held,
 * under the duplicate's Helm — no write of it comes between — and the Ink
 * lays the change only while both files are what was compared (ink.h,
 * InkConjugate): a file changed between the two, by a name, a tag, a write of
 * the target, refuses the act with nothing changed.
 *
 * ‼ DEDUP FOR EVERY FILE BUT THOSE IT MAY NOT TOUCH (§16.7 п.8). No tag of a
 * file's asks it out: its tags are its user's words, never a switch of the
 * kernel's. A file the system keeps — one its record puts in a role (system, boot
 * or kernel) — is never laid,
 * warrant or none: a loader and the plate's check read such a file's blocks
 * as they lie. And the kernel the Deed points the loader at is neither laid
 * nor laid on: its blocks are no other file's.
 *
 * ‼ ASKED ONLY WITH THE WARRANT. The act's answer says whether one file holds
 * another's bytes; a cabin that may not read the other would learn them. So
 * it is asked by a cabin wearing system or god, as Escheat is.
 *
 *   ConjugateAsk     the duplicate's `count` blocks from its block `at`, laid
 *                    on the target `onto`'s as many from its block `onto_at`
 *                    — the duplicate's must be its plain ground (DATA, no
 *                    Slip over them), the target's ground of any kind, and a
 *                    file laid on itself on blocks of its own outside the
 *                    range; at most UINT32_MAX blocks an ask, the runs a node
 *                    of Fallow counts — a longer range is asked again from
 *                    where the last ended
 *   TagFSConjugate   asked from any core with no TagFS lock, inside or out
 *                    of the door: OK and the blocks laid, or the refusal,
 *                    nothing changed, its reason in `why` — the error by the
 *                    reason: ERR_PERMISSION_DENIED (UNWARRANTED, KEPT),
 *                    ERR_INVALID_ARGUMENT (UNSHAPED, DIFFERS,
 *                    COLLIDES), ERR_CHECKSUM (UNDIGESTED), ERR_BUSY (CHANGED),
 *                    and the volume's own refusal (REFUSED):
 *                    ERR_FILE_NOT_FOUND, ERR_BUSY for a Helm
 *                    another holds, ERR_TAGFS_CORRUPTED for a file the mount
 *                    locked, ERR_READ_FAILED, ERR_NO_MEMORY, the ribbon's room
 *                    (ERR_WOULD_BLOCK while it can come) and the rest an Ink
 *                    answers
 *   ConjugateLay     the act as Dedup's pass asks it (dedup.h): the kernel's
 *                    own, with the warrant, answered as TagFSConjugate answers
 *                    and not said act by act — the pass says what it laid and
 *                    why an act was refused in its own line
 *   ConjugateStandBetween  the stand's: `between` run once, by the next act,
 *                    after its comparison and before its Ink, with no TagFS
 *                    lock held
 */

typedef enum {
    CONJUGATE_LAID = 0,     /* laid — and, inside the act, nothing refused it yet */
    CONJUGATE_UNWARRANTED,  /* the cabin asking wears neither system nor god */
    CONJUGATE_KEPT,         /* the duplicate is a file the system keeps in a role, or either is the kernel's */
    CONJUGATE_UNSHAPED,     /* the blocks asked are not the duplicate's plain ground, or not the target's */
    CONJUGATE_UNDIGESTED,   /* a block the book holds no digest for, or one that does not hold it */
    CONJUGATE_DIFFERS,      /* the digests of a pair differ */
    CONJUGATE_COLLIDES,     /* the digests of a pair agree and the bytes do not */
    CONJUGATE_CHANGED,      /* a file changed between the comparison and the Ink */
    CONJUGATE_REFUSED,      /* the volume refused: the error says why */
} ConjugateWhy;

typedef struct {
    uint64_t fid;       /* the duplicate */
    uint64_t at;        /* its first block laid */
    uint64_t onto;      /* the file whose blocks it is laid on */
    uint64_t onto_at;   /* that file's first block */
    uint64_t count;     /* blocks */
} ConjugateAsk;

typedef struct {
    uint64_t blocks;    /* the duplicate's blocks laid on the target's: `count`, or 0 */
    uint64_t at;        /* the duplicate's block a refusal met (UNSHAPED, UNDIGESTED, DIFFERS, COLLIDES) */
    uint64_t why;       /* ConjugateWhy */
} ConjugateSaid;

error_t TagFSConjugate(InkWarrant warrant, const ConjugateAsk* ask, ConjugateSaid* out);
error_t ConjugateLay(const ConjugateAsk* ask, ConjugateSaid* out);

#ifdef CONFIG_VOLUME_TESTS
void    ConjugateStandBetween(void (*between)(void* ctx), void* ctx);
#endif

#endif /* TAGFS_DEDUP_CONJUGATE_H */
