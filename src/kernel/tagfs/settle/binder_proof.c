#include "binder_internal.h"
#include "../mount/book_mount.h"

/*
 * The copies of the leaves no Tally lays again, read — the part of Self-Heal
 * the Binder carries.
 *
 * A mount reads a node's first copy and the second only when the first does
 * not hold, and a Tally laid in names a leaf nothing changed as it lies: a
 * second copy that rots would rot in silence for as long as its leaf is clean
 * — a book written whole every time laid every leaf anew and hid it. So each Tally
 * laid in, at its DONE, reads both copies of as many leaves as it laid: the
 * leaves checked longest ago, in the order of a sweep that goes round every
 * tree's leaves by key — FILES, TAGS, USE, then DIGESTS out of the reads'
 * table — from where the last left off, no number of turns or age in it; a
 * mount begins it at a point drawn from the seq of the Tally it stood on, so
 * mounts that lay few Tallies each still reach every leaf in turn (ProofBegun). A
 * leaf the Tally just laid is passed over: its copies are new. A copy that does
 * not hold puts the leaf's keys into DogEar, as a mount's walk would (§2.5):
 * the next Tally lays it anew, both copies elsewhere; nothing is written in
 * place, and nothing at all while every copy holds.
 *
 * The Settle's holder's, with no TagFS lock held: the reads go to the medium.
 */

static const uint64_t s_proof_trees[] = { TALLY_FILES, TALLY_TAGS, TALLY_USE, TALLY_DIGESTS };
#define PROOF_TREES  (sizeof(s_proof_trees) / sizeof(s_proof_trees[0]))

/* Where the sweep goes on: the tree, and the least lower key of the next leaf
 * it reads there — the tree's first leaf when it has none. */
typedef struct {
    uint32_t tree;
    bool     has_from;
    TallyKey from;
    bool     begun;
} ProofSweep;

static ProofSweep s_proof;

/* The leaves the Tally of `seq` laid — every tree's, DIGESTS's out of the
 * reads' table — and every leaf there is. */
static uint64_t ProofLaid(uint64_t seq, uint64_t* all)
{
    uint64_t   laid = 0;
    TallyPlace p;
    bool       rotten = false;
    *all = 0;
    for (uint32_t t = 0; t < PROOF_TREES; t++)
        for (uint32_t i = 0; BinderSiftLeafAt(s_proof_trees[t], i, &p, &rotten); i++) {
            *all += p.level == 0;
            laid += p.level == 0 && p.ptr.seq == seq;
        }
    return laid;
}

/* The next leaf of the sweep — a place of the reads' table above level 0 is a
 * rotten inner node's range, passed over — into *p; the sweep moved past it.
 * False when the tree it stood in had none left: it moved on to the next. */
static bool ProofNext(TallyPlace* p, bool* rotten)
{
    uint64_t tree = s_proof_trees[s_proof.tree];
    uint32_t n    = BinderSiftLeaves(tree);
    uint32_t i    = s_proof.has_from ? BinderSiftLeafFrom(tree, 1, n, s_proof.from) : 0;
    if (i >= n || !BinderSiftLeafAt(tree, i, p, rotten)) {
        s_proof.tree     = (s_proof.tree + 1u) % PROOF_TREES;
        s_proof.has_from = false;
        return false;
    }
    s_proof.has_from = p->has_hi;
    s_proof.from     = p->hi;
    if (!p->has_hi)
        s_proof.tree = (s_proof.tree + 1u) % PROOF_TREES;
    return true;
}

/*
 * Where a mount's sweep begins: a point of the ring of every tree's leaves
 * drawn from the seq of the Tally the mount stood on by the golden ratio's
 * sequence — mounts one after another begin far apart, and every leaf is
 * reached in turn however few Tallies a mount lays. No byte is written for it.
 */
static void ProofBegun(uint64_t seq)
{
    uint64_t total = 0;
    for (uint32_t t = 0; t < PROOF_TREES; t++)
        total += BinderSiftLeaves(s_proof_trees[t]);
    s_proof.begun = true;
    uint64_t at = ((seq * 0x9E3779B97F4A7C15ull) >> 32) * (total & 0xFFFFFFFFull) >> 32;
    for (uint32_t t = 0; t < PROOF_TREES; t++) {
        uint32_t   n = BinderSiftLeaves(s_proof_trees[t]);
        TallyPlace p;
        bool       rotten = false;
        if (at >= n) {
            at -= n;
            continue;
        }
        s_proof.tree     = t;
        s_proof.has_from = at > 0 && BinderSiftLeafAt(s_proof_trees[t], (uint32_t)at, &p, &rotten) && p.has_lo;
        s_proof.from     = s_proof.has_from ? p.lo : (TallyKey){ 0, 0 };
        return;
    }
}

/* Both copies of leaf `p` of `tree` read; one that does not hold said, and
 * the leaf's keys put into DogEar. False when the medium or memory refused a
 * read, said — the sweep stops for this Tally, and the next begins at it. */
static bool ProofLeaf(uint64_t tree, const TallyPlace* p)
{
    char why[112];
    for (uint32_t k = 0; k < p->ptr.copies; k++) {
        bool    holds = false;
        error_t rc    = TallyCopyHolds(p, k, &holds, why, sizeof(why));
        if (rc != OK) {
            kprintf("[TagFS] the %s leaf at block %llu: copy %u would not be read to check it (error %u) — the "
                    "check of copies stops for this Tally, and the next begins at it\n", TallyTreeName(tree),
                    (unsigned long long)TallyPtrBlock(&p->ptr, 0), k + 1u, (unsigned)rc);
            return false;
        }
        if (holds)
            continue;
        kprintf("[TagFS] the %s leaf at block %llu: copy %u does not hold — %s; the next Tally lays the leaf "
                "anew (§2.5)\n", TallyTreeName(tree), (unsigned long long)TallyPtrBlock(&p->ptr, 0), k + 1u, why);
        DogEarMended(tree, p);
        return true;
    }
    return true;
}

void BinderProofAt(const BinderRun* r)
{
    uint64_t   all = 0, want = r->w.tip ? ProofLaid(r->w.seq, &all) : 0, looked = 0;
    TallyPlace p;
    bool       rotten = false;
    if (FoliationLost())
        return;
    if (want && !s_proof.begun)
        ProofBegun(g_book.stood ? g_book.stood->head.tally_seq : 0);
    while (want && looked <= all + PROOF_TREES) {
        ProofSweep was  = s_proof;
        uint32_t   tree = s_proof.tree;
        looked++;
        if (!ProofNext(&p, &rotten) || p.level != 0 || rotten || p.ptr.seq == r->w.seq)
            continue;
        want--;
        if (!ProofLeaf(s_proof_trees[tree], &p)) {
            s_proof = was;
            return;
        }
    }
}

void BinderProofLetGo(void)
{
    memset(&s_proof, 0, sizeof(s_proof));
}
