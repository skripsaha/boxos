#include "dedup_internal.h"
#include "conjugate.h"
#include "../mount/tally_read.h"
#include "../run/run.h"
#include "../settle/binder.h"
#include "tagfs_volume.h"

/*
 * The pass (dedup.h): a Tally's blocks taken into Commonplace and held to one
 * another, the book's DIGESTS walked a leaf a turn for blocks of the same
 * digests, who names each block asked of the mirror a few files a turn, and
 * every copy laid on its original by Conjugate an act a turn. One pass at a
 * time; Tallies that end meanwhile wait their turn, and the next pass takes
 * them all.
 *
 * ‼ A DIGEST IS A CANDIDATE, AND THE ACT DECIDES (§16.7 п.8). The pass never
 * compares a byte: two blocks of one digest are handed to Conjugate, which
 * holds each pair to the book's digests and then byte for byte, under the
 * copy's Helm, and lays the copy only while both files are as it compared
 * them (conjugate.h). What the pass decides is which is the original and what
 * may be asked at all — so a block whose act is refused is counted with its
 * reason, never laid by a guess.
 */

#define DEDUP_FILES_TURN  64u      /* live files a turn of the walk of the mirror holds to the blocks asked */


static void DedupPassClear(TagFSVolume* vol)
{
    kfree(vol->dedup_pass.pass.member);
    kfree(vol->dedup_pass.pass.pair);
    DedupOwnersLetGo(&vol->dedup_pass.pass.owners);
    memset(&vol->dedup_pass.pass, 0, sizeof(vol->dedup_pass.pass));
}

void DedupPassLetGo(TagFSVolume* vol)
{
    DedupPassClear(vol);
    CommonplaceLetGo(vol);
    vol->dedup_pass.cursor = 0;
    vol->dedup_pass.walked = 0;
}

// ----------------------------------------------------------------------------
// The matches
// ----------------------------------------------------------------------------

static bool DedupMemberPut(TagFSVolume* vol, uint64_t digest, uint64_t block, uint32_t old)
{
    if (vol->dedup_pass.pass.nmember == vol->dedup_pass.pass.capmember) {
        uint64_t     cap  = vol->dedup_pass.pass.capmember ? vol->dedup_pass.pass.capmember * 2u : 64u;
        DedupMember* more = cap <= UINT64_MAX / sizeof(DedupMember) ? kmalloc(sizeof(DedupMember) * cap) : NULL;
        if (!more)
            return false;
        if (vol->dedup_pass.pass.nmember)
            memcpy(more, vol->dedup_pass.pass.member, sizeof(DedupMember) * vol->dedup_pass.pass.nmember);
        kfree(vol->dedup_pass.pass.member);
        vol->dedup_pass.pass.member    = more;
        vol->dedup_pass.pass.capmember = cap;
    }
    vol->dedup_pass.pass.member[vol->dedup_pass.pass.nmember++] = (DedupMember){ .digest = digest, .block = block, .old = old };
    return true;
}

/* The `count` entries of a digest among the pass's matches, once. */
static bool DedupGroupIn(TagFSVolume* vol, CommonplaceEntry* g, uint64_t count)
{
    if (g->mark & COMMONPLACE_IN)
        return true;
    for (uint64_t k = 0; k < count; k++)
        if (!DedupMemberPut(vol, g[k].digest, g[k].block, 0))
            return false;
    g->mark |= COMMONPLACE_IN;
    return true;
}

/* Every digest two blocks of Commonplace share, one of them come in with this
 * pass, among the matches: false with no memory. */
static bool DedupAmong(TagFSVolume* vol)
{
    uint64_t n = CommonplaceCount(vol);
    for (uint64_t i = 0, j = 0; i < n; i = j) {
        CommonplaceEntry* g     = CommonplaceAt(vol, i);
        bool              fresh = false;
        for (j = i; j < n && CommonplaceAt(vol, j)->digest == g->digest; j++)
            fresh |= CommonplaceAt(vol, j)->fresh != 0;
        if (j - i >= 2u && fresh && !DedupGroupIn(vol, g, j - i))
            return false;
    }
    return true;
}

/* A run of digested blocks a leaf of DIGESTS names: a block whose digest
 * Commonplace holds for another block is matched to them — the first such
 * block a pass, for a digest — and a block of Commonplace itself is no match
 * of its own. *ctx no memory. */
static void DedupWalkRun(TagFSVolume* vol, void* ctx, uint64_t start, uint64_t n, const uint8_t* digests)
{
    bool* refused = ctx;
    for (uint64_t i = 0; i < n && !*refused; i++) {
        uint64_t          count = 0, digest = TallyDigestAt(digests, i), block = start + i;
        CommonplaceEntry* g     = CommonplaceGroup(vol, digest, &count);
        bool              own   = false;
        for (uint64_t k = 0; g && k < count; k++)
            own |= g[k].block == block;
        if (!g || own || (g->mark & COMMONPLACE_OLD))
            continue;
        if (!DedupGroupIn(vol, g, count) || !DedupMemberPut(vol, digest, block, 1u)) {
            *refused = true;
            return;
        }
        g->mark |= COMMONPLACE_OLD;
    }
}

// ----------------------------------------------------------------------------
// Copies and their originals
// ----------------------------------------------------------------------------

static int MemberOrder(const void* a, const void* b, void* ctx)
{
    const DedupMember* x = a;
    const DedupMember* y = b;
    (void)ctx;
    if (x->digest != y->digest)
        return x->digest < y->digest ? -1 : 1;
    return x->block < y->block ? -1 : x->block > y->block;
}

static int PairOrder(const void* a, const void* b, void* ctx)
{
    const DedupPair* x = a;
    const DedupPair* y = b;
    (void)ctx;
    if (x->fid != y->fid)
        return x->fid < y->fid ? -1 : 1;
    return x->at < y->at ? -1 : x->at > y->at;
}

/* Whether `a` is older than `b`: its run born in an earlier stitch, its file
 * made first, the earlier block of it. */
static bool OwnerOlder(const DedupOwner* a, const DedupOwner* b)
{
    if (a->birth != b->birth)
        return a->birth < b->birth;
    if (a->fid != b->fid)
        return a->fid < b->fid;
    return a->at < b->at;
}

/* The original of a digest's `count` members: the oldest a file names that
 * nothing bars being laid on — its owner in *to — or NULL. */
static const DedupMember* DedupOriginal(TagFSVolume* vol, const DedupMember* g, uint64_t count, const DedupOwner** to)
{
    const DedupMember* t = NULL;
    *to                  = NULL;
    for (uint64_t k = 0; k < count; k++) {
        const DedupOwner* o = DedupOwnerOf(&vol->dedup_pass.pass.owners, g[k].block);
        if (!o || o->kind == DEDUP_NO_OWNER || (o->bars & DEDUP_BAR_ONTO))
            continue;
        if (!t || OwnerOlder(o, *to)) {
            t   = &g[k];
            *to = o;
        }
    }
    return t;
}

/* A digest's members held to their owners: every block of Commonplace but the
 * original, its file's plain ground nothing bars being laid and its file not
 * the original's, a copy laid on the original; the rest passed over. */
static void DedupGroupPaired(TagFSVolume* vol, const DedupMember* g, uint64_t count)
{
    const DedupOwner*  to = NULL;
    const DedupMember* t  = DedupOriginal(vol, g, count, &to);
    for (uint64_t k = 0; k < count; k++) {
        const DedupOwner* o = DedupOwnerOf(&vol->dedup_pass.pass.owners, g[k].block);
        if (&g[k] == t || g[k].old)
            continue;
        if (!t || !o || o->kind != STRIP_DATA || (o->bars & DEDUP_BAR_COPY) || o->fid == to->fid) {
            vol->dedup_pass.pass.said.passed++;
            continue;
        }
        vol->dedup_pass.pass.pair[vol->dedup_pass.pass.npair++] = (DedupPair){ .fid = o->fid, .at = o->at, .onto = to->fid,
                                                   .onto_at = to->at, .digest = g[k].digest,
                                                   .block = g[k].block, .book = t->old };
        if (t->old)
            vol->dedup_pass.pass.said.book++;
        else
            vol->dedup_pass.pass.said.among++;
    }
}

/* Whether pair `q` goes on the series pair `p` ends: the copy's next block of
 * its file laid on the original's next block of its own. */
static bool DedupPairFollows(const DedupPair* p, const DedupPair* q)
{
    return q->fid == p->fid && q->at == p->at + 1u && q->onto == p->onto && q->onto_at == p->onto_at + 1u;
}

/* The pairs of the series that begins at pair `first`. */
static uint64_t DedupSeriesFrom(TagFSVolume* vol, uint64_t first)
{
    uint64_t n = 1;
    while (first + n < vol->dedup_pass.pass.npair && DedupPairFollows(&vol->dedup_pass.pass.pair[first + n - 1u], &vol->dedup_pass.pass.pair[first + n]))
        n++;
    return n;
}

/* Every series shorter than DEDUP_SERIES_LEAST left as it is (dedup.h): its
 * pairs out, counted. */
static void DedupSeriesKept(TagFSVolume* vol)
{
    uint64_t kept = 0;
    for (uint64_t i = 0, n = 0; i < vol->dedup_pass.pass.npair; i += n) {
        n = DedupSeriesFrom(vol, i);
        if (n < DEDUP_SERIES_LEAST) {
            vol->dedup_pass.pass.said.scant += n;
            continue;
        }
        memmove(&vol->dedup_pass.pass.pair[kept], &vol->dedup_pass.pass.pair[i], sizeof(DedupPair) * n);
        kept += n;
    }
    vol->dedup_pass.pass.npair = kept;
}

/* Every copy paired with its original, in the order of the copies' files and
 * their blocks, and only those of a series kept: false with no memory. */
static bool DedupPairsMade(TagFSVolume* vol)
{
    const DedupMember* m = vol->dedup_pass.pass.member;
    uint64_t           n = vol->dedup_pass.pass.nmember;
    vol->dedup_pass.pass.pair          = kmalloc(sizeof(DedupPair) * n);
    if (!vol->dedup_pass.pass.pair)
        return false;
    for (uint64_t i = 0, j = 0; i < n; i = j) {
        for (j = i; j < n && m[j].digest == m[i].digest; j++)
            ;
        DedupGroupPaired(vol, &m[i], j - i);
    }
    KSort(vol->dedup_pass.pass.pair, vol->dedup_pass.pass.npair, sizeof(DedupPair), PairOrder, NULL);
    DedupSeriesKept(vol);
    return true;
}

// ----------------------------------------------------------------------------
// The pass's steps
// ----------------------------------------------------------------------------

static const char* const s_why[] = {
    [CONJUGATE_LAID]        = "laid",
    [CONJUGATE_UNWARRANTED] = "unwarranted",
    [CONJUGATE_KEPT]        = "kept",
    [CONJUGATE_UNSHAPED]    = "not plain ground",
    [CONJUGATE_UNDIGESTED]  = "no digest that holds",
    [CONJUGATE_DIFFERS]     = "digests that differ",
    [CONJUGATE_COLLIDES]    = "bytes that differ",
    [CONJUGATE_CHANGED]     = "a file changed meanwhile",
    [CONJUGATE_REFUSED]     = "the volume's refusal",
};

/* The pass's line: said only when it matched a block. */
static void DedupPassSay(TagFSVolume* vol)
{
    const DedupSaid* s = &vol->dedup_pass.pass.said;
    char             why[160] = "", walk[96] = "";
    size_t           at       = 0;
    if (s->among + s->book + s->passed == 0 && vol->dedup_pass.pass.walk_rc == OK)
        return;
    for (uint32_t r = CONJUGATE_UNWARRANTED; r <= CONJUGATE_REFUSED && at < sizeof(why); r++)
        if (vol->dedup_pass.pass.why[r])
            at += (size_t)ksnprintf(why + at, sizeof(why) - at, "%s%llu %s", at ? ", " : " — ",
                                    (unsigned long long)vol->dedup_pass.pass.why[r], s_why[r]);
    if (vol->dedup_pass.pass.walk_rc != OK)
        ksnprintf(walk, sizeof(walk), ", stopped there for this pass (error %u)", (unsigned)vol->dedup_pass.pass.walk_rc);
    kprintf("[TagFS] Dedup after Tally %llu: %llu block(s) written since the Tally before weighed; %llu matched to "
            "one written beside it and %llu to one the book's DIGESTS names; %llu block(s) laid on the blocks they "
            "duplicate, %llu in series shorter than %u left as they are, %llu passed over (a file the system keeps, "
            "a copy in its original's own file, a block no file names), %llu refused%s; the walk of DIGESTS at "
            "block %llu%s, %llu block(s) kept in Commonplace\n",
            (unsigned long long)vol->dedup_pass.pass.tally, (unsigned long long)s->weighed, (unsigned long long)s->among, (unsigned long long)s->book,
            (unsigned long long)s->laid, (unsigned long long)s->scant, DEDUP_SERIES_LEAST, (unsigned long long)s->passed,
            (unsigned long long)s->refused, why, (unsigned long long)vol->dedup_pass.cursor, walk, (unsigned long long)CommonplaceCount(vol));
}

/* The pass ended: Commonplace past what it laid and what the walk went round
 * the book for, what it keeps published, and said. Listed again only by a
 * batch waiting. */
static bool DedupPassEnd(TagFSVolume* vol)
{
    CommonplaceRetire(vol, vol->dedup_pass.walked, vol->state.block_end);
    DedupHolding(vol, CommonplaceCount(vol));
    DedupPassSay(vol);
#ifdef CONFIG_VOLUME_TESTS
    DedupPassSaid(vol, &vol->dedup_pass.pass.said);
#endif
    DedupPassClear(vol);
    return false;
}

/* Every batch handed over taken into Commonplace and held to what it held: a
 * pass begun. Held down, the batches and Commonplace let go instead. */
static bool DedupPassBegin(TagFSVolume* vol)
{
    DedupBatch* b = DedupPendingTake(vol);
    if (DedupHeld(vol) || !b) {
        while (b) {
            DedupBatch* next = b->next;
            DedupBatchLetGo(b);
            b = next;
        }
        if (DedupHeld(vol)) {
            CommonplaceLetGo(vol);
            DedupHolding(vol, 0);
        }
        return false;
    }
    vol->dedup_pass.pass.began = vol->dedup_pass.walked;
    while (b) {
        DedupBatch* next = b->next;
        vol->dedup_pass.pass.tally   = b->tally;
        vol->dedup_pass.pass.budget += b->budget;
        if (CommonplaceTake(vol, b->entry, b->n, b->gone, b->ngone, vol->dedup_pass.walked) == OK)
            vol->dedup_pass.pass.said.weighed += b->n;
        else
            kprintf("[TagFS] Dedup: no memory to take the %llu block(s) Tally %llu took in into Commonplace — "
                    "none of them is weighed\n", (unsigned long long)b->n, (unsigned long long)b->tally);
        DedupBatchLetGo(b);
        b = next;
    }
    if (!DedupAmong(vol))
        vol->dedup_pass.pass.walk_rc = ERR_NO_MEMORY;
    vol->dedup_pass.pass.step = DEDUP_WALK;
    return true;
}

/* The walk ended — its budget spent, the book gone round for every block the
 * pass weighs, or a refusal: the members sorted by digest and the mirror asked
 * who names them. */
static bool DedupWalkEnded(TagFSVolume* vol)
{
    if (vol->dedup_pass.pass.nmember == 0)
        return DedupPassEnd(vol);
    KSort(vol->dedup_pass.pass.member, vol->dedup_pass.pass.nmember, sizeof(DedupMember), MemberOrder, NULL);
    if (DedupOwnersBegin(&vol->dedup_pass.pass.owners, vol->dedup_pass.pass.member, vol->dedup_pass.pass.nmember) != OK) {
        kprintf("[TagFS] Dedup: no memory to ask who names the %llu block(s) Tally %llu's pass matched — none of "
                "them is laid\n", (unsigned long long)vol->dedup_pass.pass.nmember, (unsigned long long)vol->dedup_pass.pass.tally);
        return DedupPassEnd(vol);
    }
    vol->dedup_pass.pass.step = DEDUP_OWNERS;
    return true;
}

/* One leaf of the book's DIGESTS read from the walk's place, while the pass
 * may read more and a block of Commonplace has the book still to go round. */
static bool DedupWalkStep(TagFSVolume* vol)
{
    uint64_t lap = vol->state.block_end, next = 0, sheets = 0;
    bool     refused = false;
    if (vol->dedup_pass.pass.walk_rc != OK || vol->dedup_pass.pass.budget == 0 || CommonplaceCount(vol) == 0 || vol->dedup_pass.walked - vol->dedup_pass.pass.began >= lap)
        return DedupWalkEnded(vol);
    error_t rc = TallyDigestsFrom(vol, vol->dedup_pass.cursor, DedupWalkRun, &refused, &next, &sheets);
    if (rc != OK || refused) {
        vol->dedup_pass.pass.walk_rc = rc != OK ? rc : ERR_NO_MEMORY;
        return true;
    }
    uint64_t to = next < lap ? next : lap;
    vol->dedup_pass.walked     += to > vol->dedup_pass.cursor ? to - vol->dedup_pass.cursor : 0;
    vol->dedup_pass.cursor      = next < lap ? next : 0;
    vol->dedup_pass.pass.budget = sheets < vol->dedup_pass.pass.budget ? vol->dedup_pass.pass.budget - sheets : 0;
    return true;
}

/* A few files of the mirror held to the blocks matched; once every one is,
 * the copies paired with their originals. */
static bool DedupOwnersTurn(TagFSVolume* vol)
{
    if (!DedupOwnersStep(vol, &vol->dedup_pass.pass.owners, DEDUP_FILES_TURN))
        return true;
    if (!DedupPairsMade(vol)) {
        kprintf("[TagFS] Dedup: no memory to pair the %llu block(s) Tally %llu's pass matched — none of them is "
                "laid\n", (unsigned long long)vol->dedup_pass.pass.nmember, (unsigned long long)vol->dedup_pass.pass.tally);
        return DedupPassEnd(vol);
    }
    DedupOwnersLetGo(&vol->dedup_pass.pass.owners);
    kfree(vol->dedup_pass.pass.member);
    vol->dedup_pass.pass.member = NULL;
    vol->dedup_pass.pass.nmember = vol->dedup_pass.pass.capmember = 0;
    vol->dedup_pass.pass.step = DEDUP_LAY;
    return true;
}

/* How many pairs from `first` one act lays: of the series they begin, as many
 * as the seat takes in one command — fewer where the rest would then be
 * shorter than a series and the series can give both their share. */
static uint64_t DedupAskRun(TagFSVolume* vol, uint64_t first)
{
    uint64_t rest = DedupSeriesFrom(vol, first), most = TagFSRunBlocks(vol);
    if (rest <= most)
        return rest;
    if (rest - most < DEDUP_SERIES_LEAST && rest >= 2u * DEDUP_SERIES_LEAST)
        return rest - DEDUP_SERIES_LEAST;
    return most;
}

/* One act: the copies of the next run laid on their original. The ribbon's
 * room on its way waits for the cycle that brings it (DedupWeigh); a volume
 * that takes no write ends the pass. */
static bool DedupLayStep(TagFSVolume* vol)
{
    if (vol->dedup_pass.pass.ask >= vol->dedup_pass.pass.npair || BinderNoWrite(vol))
        return DedupPassEnd(vol);
    const DedupPair* p   = &vol->dedup_pass.pass.pair[vol->dedup_pass.pass.ask];
    uint64_t         n   = DedupAskRun(vol, vol->dedup_pass.pass.ask);
    ConjugateAsk     ask = { .fid = p->fid, .at = p->at, .onto = p->onto, .onto_at = p->onto_at, .count = n };
    ConjugateSaid    said;
    error_t          rc = ConjugateLay(vol, &ask, &said);
    if (rc == ERR_WOULD_BLOCK) {
        DedupRoomWait(vol);
        return false;
    }
    if (rc == OK) {
        for (uint64_t k = 0; k < n; k++)
            CommonplaceStrike(vol, p[k].digest, p[k].block);
        vol->dedup_pass.pass.said.laid += n;
    } else {
        vol->dedup_pass.pass.said.refused += n;
        vol->dedup_pass.pass.why[said.why <= CONJUGATE_REFUSED ? said.why : CONJUGATE_REFUSED] += n;
        if (rc == ERR_BUSY)
            for (uint64_t k = 0; k < n; k++)
                CommonplaceKeep(vol, p[k].digest, p[k].block, vol->dedup_pass.walked);
    }
    vol->dedup_pass.pass.ask += n;
    return true;
}

bool DedupPassTurn(TagFSVolume* vol)
{
    uint64_t epoch = tagfs_mount_epoch(vol);
    if (vol->dedup_pass.epoch != epoch) {
        DedupPassLetGo(vol);
        vol->dedup_pass.epoch = epoch;
    }
    switch (vol->dedup_pass.pass.step) {
    case DEDUP_WALK:
        return DedupWalkStep(vol);
    case DEDUP_OWNERS:
        return DedupOwnersTurn(vol);
    case DEDUP_LAY:
        return DedupLayStep(vol);
    default:
        return DedupPassBegin(vol);
    }
}
