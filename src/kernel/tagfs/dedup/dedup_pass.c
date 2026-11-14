#include "dedup_internal.h"
#include "conjugate.h"
#include "../mount/tally_read.h"
#include "../run/run.h"
#include "../settle/binder.h"

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

typedef enum {
    DEDUP_IDLE = 0,
    DEDUP_WALK,          /* the book's DIGESTS, a leaf a turn */
    DEDUP_OWNERS,        /* the mirror, a few files a turn */
    DEDUP_LAY,           /* an act a turn */
} DedupStep;

/* A copy and its original: the copy's file and its block of it, the
 * original's file and block, and the copy's block itself by its digest —
 * struck from Commonplace once laid. `book`: the original was read off
 * DIGESTS. */
typedef struct {
    uint64_t fid;
    uint64_t at;
    uint64_t onto;
    uint64_t onto_at;
    uint64_t digest;
    uint64_t block;
    uint32_t book;
    uint32_t pad;
} DedupPair;

typedef struct {
    DedupStep    step;
    uint64_t     tally;              /* the newest Tally whose blocks the pass weighs */
    uint64_t     budget;             /* Sheets of DIGESTS the walk may read yet */
    uint64_t     began;              /* where the walk stood when the pass's blocks came in */
    error_t      walk_rc;            /* the refusal that stopped the walk, OK for none */
    DedupMember* member;
    uint64_t     nmember;
    uint64_t     capmember;
    DedupOwners  owners;
    DedupPair*   pair;
    uint64_t     npair;
    uint64_t     ask;                /* the pair the next act begins at */
    DedupSaid    said;
    uint64_t     why[CONJUGATE_REFUSED + 1u];
} DedupPass;

static DedupPass s_pass;
static uint64_t  s_cursor;           /* the block the walk of DIGESTS reads on from */
static uint64_t  s_walked;           /* blocks the walk went past since the mount: a lap is the volume's */
static uint64_t  s_epoch;

static void DedupPassClear(void)
{
    kfree(s_pass.member);
    kfree(s_pass.pair);
    DedupOwnersLetGo(&s_pass.owners);
    memset(&s_pass, 0, sizeof(s_pass));
}

void DedupPassLetGo(void)
{
    DedupPassClear();
    CommonplaceLetGo();
    s_cursor = 0;
    s_walked = 0;
}

// ----------------------------------------------------------------------------
// The matches
// ----------------------------------------------------------------------------

static bool DedupMemberPut(uint64_t digest, uint64_t block, uint32_t old)
{
    if (s_pass.nmember == s_pass.capmember) {
        uint64_t     cap  = s_pass.capmember ? s_pass.capmember * 2u : 64u;
        DedupMember* more = cap <= UINT64_MAX / sizeof(DedupMember) ? kmalloc(sizeof(DedupMember) * cap) : NULL;
        if (!more)
            return false;
        if (s_pass.nmember)
            memcpy(more, s_pass.member, sizeof(DedupMember) * s_pass.nmember);
        kfree(s_pass.member);
        s_pass.member    = more;
        s_pass.capmember = cap;
    }
    s_pass.member[s_pass.nmember++] = (DedupMember){ .digest = digest, .block = block, .old = old };
    return true;
}

/* The `count` entries of a digest among the pass's matches, once. */
static bool DedupGroupIn(CommonplaceEntry* g, uint64_t count)
{
    if (g->mark & COMMONPLACE_IN)
        return true;
    for (uint64_t k = 0; k < count; k++)
        if (!DedupMemberPut(g[k].digest, g[k].block, 0))
            return false;
    g->mark |= COMMONPLACE_IN;
    return true;
}

/* Every digest two blocks of Commonplace share, one of them come in with this
 * pass, among the matches: false with no memory. */
static bool DedupAmong(void)
{
    uint64_t n = CommonplaceCount();
    for (uint64_t i = 0, j = 0; i < n; i = j) {
        CommonplaceEntry* g     = CommonplaceAt(i);
        bool              fresh = false;
        for (j = i; j < n && CommonplaceAt(j)->digest == g->digest; j++)
            fresh |= CommonplaceAt(j)->fresh != 0;
        if (j - i >= 2u && fresh && !DedupGroupIn(g, j - i))
            return false;
    }
    return true;
}

/* A run of digested blocks a leaf of DIGESTS names: a block whose digest
 * Commonplace holds for another block is matched to them — the first such
 * block a pass, for a digest — and a block of Commonplace itself is no match
 * of its own. *ctx no memory. */
static void DedupWalkRun(void* ctx, uint64_t start, uint64_t n, const uint8_t* digests)
{
    bool* refused = ctx;
    for (uint64_t i = 0; i < n && !*refused; i++) {
        uint64_t          count = 0, digest = TallyDigestAt(digests, i), block = start + i;
        CommonplaceEntry* g     = CommonplaceGroup(digest, &count);
        bool              own   = false;
        for (uint64_t k = 0; g && k < count; k++)
            own |= g[k].block == block;
        if (!g || own || (g->mark & COMMONPLACE_OLD))
            continue;
        if (!DedupGroupIn(g, count) || !DedupMemberPut(digest, block, 1u)) {
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
static const DedupMember* DedupOriginal(const DedupMember* g, uint64_t count, const DedupOwner** to)
{
    const DedupMember* t = NULL;
    *to                  = NULL;
    for (uint64_t k = 0; k < count; k++) {
        const DedupOwner* o = DedupOwnerOf(&s_pass.owners, g[k].block);
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
static void DedupGroupPaired(const DedupMember* g, uint64_t count)
{
    const DedupOwner*  to = NULL;
    const DedupMember* t  = DedupOriginal(g, count, &to);
    for (uint64_t k = 0; k < count; k++) {
        const DedupOwner* o = DedupOwnerOf(&s_pass.owners, g[k].block);
        if (&g[k] == t || g[k].old)
            continue;
        if (!t || !o || o->kind != STRIP_DATA || (o->bars & DEDUP_BAR_COPY) || o->fid == to->fid) {
            s_pass.said.passed++;
            continue;
        }
        s_pass.pair[s_pass.npair++] = (DedupPair){ .fid = o->fid, .at = o->at, .onto = to->fid,
                                                   .onto_at = to->at, .digest = g[k].digest,
                                                   .block = g[k].block, .book = t->old };
        if (t->old)
            s_pass.said.book++;
        else
            s_pass.said.among++;
    }
}

/* Whether pair `q` goes on the series pair `p` ends: the copy's next block of
 * its file laid on the original's next block of its own. */
static bool DedupPairFollows(const DedupPair* p, const DedupPair* q)
{
    return q->fid == p->fid && q->at == p->at + 1u && q->onto == p->onto && q->onto_at == p->onto_at + 1u;
}

/* The pairs of the series that begins at pair `first`. */
static uint64_t DedupSeriesFrom(uint64_t first)
{
    uint64_t n = 1;
    while (first + n < s_pass.npair && DedupPairFollows(&s_pass.pair[first + n - 1u], &s_pass.pair[first + n]))
        n++;
    return n;
}

/* Every series shorter than DEDUP_SERIES_LEAST left as it is (dedup.h): its
 * pairs out, counted. */
static void DedupSeriesKept(void)
{
    uint64_t kept = 0;
    for (uint64_t i = 0, n = 0; i < s_pass.npair; i += n) {
        n = DedupSeriesFrom(i);
        if (n < DEDUP_SERIES_LEAST) {
            s_pass.said.scant += n;
            continue;
        }
        memmove(&s_pass.pair[kept], &s_pass.pair[i], sizeof(DedupPair) * n);
        kept += n;
    }
    s_pass.npair = kept;
}

/* Every copy paired with its original, in the order of the copies' files and
 * their blocks, and only those of a series kept: false with no memory. */
static bool DedupPairsMade(void)
{
    const DedupMember* m = s_pass.member;
    uint64_t           n = s_pass.nmember;
    s_pass.pair          = kmalloc(sizeof(DedupPair) * n);
    if (!s_pass.pair)
        return false;
    for (uint64_t i = 0, j = 0; i < n; i = j) {
        for (j = i; j < n && m[j].digest == m[i].digest; j++)
            ;
        DedupGroupPaired(&m[i], j - i);
    }
    KSort(s_pass.pair, s_pass.npair, sizeof(DedupPair), PairOrder, NULL);
    DedupSeriesKept();
    return true;
}

// ----------------------------------------------------------------------------
// The pass's steps
// ----------------------------------------------------------------------------

static const char* const s_why[] = {
    [CONJUGATE_LAID]        = "laid",
    [CONJUGATE_UNWARRANTED] = "unwarranted",
    [CONJUGATE_KEPT]        = "kept",
    [CONJUGATE_DECLINED]    = "dedup:off",
    [CONJUGATE_UNSHAPED]    = "not plain ground",
    [CONJUGATE_UNDIGESTED]  = "no digest that holds",
    [CONJUGATE_DIFFERS]     = "digests that differ",
    [CONJUGATE_COLLIDES]    = "bytes that differ",
    [CONJUGATE_CHANGED]     = "a file changed meanwhile",
    [CONJUGATE_REFUSED]     = "the volume's refusal",
};

/* The pass's line: said only when it matched a block. */
static void DedupPassSay(void)
{
    const DedupSaid* s = &s_pass.said;
    char             why[160] = "", walk[96] = "";
    size_t           at       = 0;
    if (s->among + s->book + s->passed == 0 && s_pass.walk_rc == OK)
        return;
    for (uint32_t r = CONJUGATE_UNWARRANTED; r <= CONJUGATE_REFUSED && at < sizeof(why); r++)
        if (s_pass.why[r])
            at += (size_t)ksnprintf(why + at, sizeof(why) - at, "%s%llu %s", at ? ", " : " — ",
                                    (unsigned long long)s_pass.why[r], s_why[r]);
    if (s_pass.walk_rc != OK)
        ksnprintf(walk, sizeof(walk), ", stopped there for this pass (error %u)", (unsigned)s_pass.walk_rc);
    kprintf("[TagFS] Dedup after Tally %llu: %llu block(s) written since the Tally before weighed; %llu matched to "
            "one written beside it and %llu to one the book's DIGESTS names; %llu block(s) laid on the blocks they "
            "duplicate, %llu in series shorter than %u left as they are, %llu passed over (a file kept or wearing "
            "dedup:off, a copy in its original's own file, a block no file names), %llu refused%s; the walk of "
            "DIGESTS at block %llu%s, %llu block(s) kept in Commonplace\n",
            (unsigned long long)s_pass.tally, (unsigned long long)s->weighed, (unsigned long long)s->among, (unsigned long long)s->book,
            (unsigned long long)s->laid, (unsigned long long)s->scant, DEDUP_SERIES_LEAST, (unsigned long long)s->passed,
            (unsigned long long)s->refused, why, (unsigned long long)s_cursor, walk, (unsigned long long)CommonplaceCount());
}

/* The pass ended: Commonplace past what it laid and what the walk went round
 * the book for, what it keeps published, and said. Listed again only by a
 * batch waiting. */
static bool DedupPassEnd(void)
{
    CommonplaceRetire(s_walked, tagfs_get_state()->block_end);
    DedupHolding(CommonplaceCount());
    DedupPassSay();
#ifdef CONFIG_VOLUME_TESTS
    DedupPassSaid(&s_pass.said);
#endif
    DedupPassClear();
    return false;
}

/* Every batch handed over taken into Commonplace and held to what it held: a
 * pass begun. Held down, the batches and Commonplace let go instead. */
static bool DedupPassBegin(void)
{
    DedupBatch* b = DedupPendingTake();
    if (DedupHeld() || !b) {
        while (b) {
            DedupBatch* next = b->next;
            DedupBatchLetGo(b);
            b = next;
        }
        if (DedupHeld()) {
            CommonplaceLetGo();
            DedupHolding(0);
        }
        return false;
    }
    s_pass.began = s_walked;
    while (b) {
        DedupBatch* next = b->next;
        s_pass.tally   = b->tally;
        s_pass.budget += b->budget;
        if (CommonplaceTake(b->entry, b->n, b->gone, b->ngone, s_walked) == OK)
            s_pass.said.weighed += b->n;
        else
            kprintf("[TagFS] Dedup: no memory to take the %llu block(s) Tally %llu took in into Commonplace — "
                    "none of them is weighed\n", (unsigned long long)b->n, (unsigned long long)b->tally);
        DedupBatchLetGo(b);
        b = next;
    }
    if (!DedupAmong())
        s_pass.walk_rc = ERR_NO_MEMORY;
    s_pass.step = DEDUP_WALK;
    return true;
}

/* The walk ended — its budget spent, the book gone round for every block the
 * pass weighs, or a refusal: the members sorted by digest and the mirror asked
 * who names them. */
static bool DedupWalkEnded(void)
{
    if (s_pass.nmember == 0)
        return DedupPassEnd();
    KSort(s_pass.member, s_pass.nmember, sizeof(DedupMember), MemberOrder, NULL);
    if (DedupOwnersBegin(&s_pass.owners, s_pass.member, s_pass.nmember) != OK) {
        kprintf("[TagFS] Dedup: no memory to ask who names the %llu block(s) Tally %llu's pass matched — none of "
                "them is laid\n", (unsigned long long)s_pass.nmember, (unsigned long long)s_pass.tally);
        return DedupPassEnd();
    }
    s_pass.step = DEDUP_OWNERS;
    return true;
}

/* One leaf of the book's DIGESTS read from the walk's place, while the pass
 * may read more and a block of Commonplace has the book still to go round. */
static bool DedupWalkStep(void)
{
    uint64_t lap = tagfs_get_state()->block_end, next = 0, sheets = 0;
    bool     refused = false;
    if (s_pass.walk_rc != OK || s_pass.budget == 0 || CommonplaceCount() == 0 || s_walked - s_pass.began >= lap)
        return DedupWalkEnded();
    error_t rc = TallyDigestsFrom(s_cursor, DedupWalkRun, &refused, &next, &sheets);
    if (rc != OK || refused) {
        s_pass.walk_rc = rc != OK ? rc : ERR_NO_MEMORY;
        return true;
    }
    uint64_t to = next < lap ? next : lap;
    s_walked     += to > s_cursor ? to - s_cursor : 0;
    s_cursor      = next < lap ? next : 0;
    s_pass.budget = sheets < s_pass.budget ? s_pass.budget - sheets : 0;
    return true;
}

/* A few files of the mirror held to the blocks matched; once every one is,
 * the copies paired with their originals. */
static bool DedupOwnersTurn(void)
{
    if (!DedupOwnersStep(&s_pass.owners, DEDUP_FILES_TURN))
        return true;
    if (!DedupPairsMade()) {
        kprintf("[TagFS] Dedup: no memory to pair the %llu block(s) Tally %llu's pass matched — none of them is "
                "laid\n", (unsigned long long)s_pass.nmember, (unsigned long long)s_pass.tally);
        return DedupPassEnd();
    }
    DedupOwnersLetGo(&s_pass.owners);
    kfree(s_pass.member);
    s_pass.member = NULL;
    s_pass.nmember = s_pass.capmember = 0;
    s_pass.step = DEDUP_LAY;
    return true;
}

/* How many pairs from `first` one act lays: of the series they begin, as many
 * as the seat takes in one command — fewer where the rest would then be
 * shorter than a series and the series can give both their share. */
static uint64_t DedupAskRun(uint64_t first)
{
    uint64_t rest = DedupSeriesFrom(first), most = TagFSRunBlocks();
    if (rest <= most)
        return rest;
    if (rest - most < DEDUP_SERIES_LEAST && rest >= 2u * DEDUP_SERIES_LEAST)
        return rest - DEDUP_SERIES_LEAST;
    return most;
}

/* One act: the copies of the next run laid on their original. The ribbon's
 * room on its way waits for the cycle that brings it (DedupWeigh); a volume
 * that takes no write ends the pass. */
static bool DedupLayStep(void)
{
    if (s_pass.ask >= s_pass.npair || BinderNoWrite())
        return DedupPassEnd();
    const DedupPair* p   = &s_pass.pair[s_pass.ask];
    uint64_t         n   = DedupAskRun(s_pass.ask);
    ConjugateAsk     ask = { .fid = p->fid, .at = p->at, .onto = p->onto, .onto_at = p->onto_at, .count = n };
    ConjugateSaid    said;
    error_t          rc = ConjugateLay(&ask, &said);
    if (rc == ERR_WOULD_BLOCK) {
        DedupRoomWait();
        return false;
    }
    if (rc == OK) {
        for (uint64_t k = 0; k < n; k++)
            CommonplaceStrike(p[k].digest, p[k].block);
        s_pass.said.laid += n;
    } else {
        s_pass.said.refused += n;
        s_pass.why[said.why <= CONJUGATE_REFUSED ? said.why : CONJUGATE_REFUSED] += n;
    }
    s_pass.ask += n;
    return true;
}

bool DedupPassTurn(void)
{
    uint64_t epoch = tagfs_mount_epoch();
    if (s_epoch != epoch) {
        DedupPassLetGo();
        s_epoch = epoch;
    }
    switch (s_pass.step) {
    case DEDUP_WALK:
        return DedupWalkStep();
    case DEDUP_OWNERS:
        return DedupOwnersTurn();
    case DEDUP_LAY:
        return DedupLayStep();
    default:
        return DedupPassBegin();
    }
}
