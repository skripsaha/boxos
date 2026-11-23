#include "conjugate.h"
#include "../ink/ink.h"
#include "../file_map/file_map.h"
#include "../file_map/tail_slip.h"
#include "../helm/helm.h"
#include "../integrity/gloss.h"
#include "../mount/book_mount.h"
#include "../mount/tally_read.h"
#include "../run/run.h"
#include "../tag_registry/tag_registry.h"
#include "../box_hash/box_hash.h"
#include "tagfs_volume.h"

/*
 * Conjugate (conjugate.h): the act asks every guard of the two files' records
 * as the duplicate's Helm holds them, walks the range into pairs of runs —
 * one run on the medium on either side — and the ground either names there,
 * holds each pair to its digests and then to its bytes, and hands what it
 * compared to Ink (ink.h, InkConjugate), which lays it only while both records
 * are the ones compared.
 *
 * ‼ THE GUARDS ARE THE RECORDS', AND THE INK HOLDS THE RECORDS. The role a
 * file is kept in, the runs it names — each is asked of the
 * record the act compared, and each changes only by an Ink of that file, whose
 * gen the lock then finds past the one the act holds it to. And a block the
 * target names stays named, its bytes and its digest with it, for as long as
 * its record stands: a block a record names is never written,
 * and goes back to the allocator only once a change of its file let it go. So
 * no digest is asked under the lock — the DIGESTS leaf that names a block may
 * have to be read off the medium (tally_read.h, TallyDigestFind) — and the
 * comparison done before it holds at the Ink.
 */

/* A pair of runs compared in one go: `count` of the duplicate's blocks from
 * `dup` and the target's from `onto` — each one run on the medium — the
 * duplicate's block of its file `dup` is at `at`. */
typedef struct {
    uint64_t dup;
    uint64_t onto;
    uint64_t at;
    uint32_t count;
} ConjugatePair;

/* The range as the act walked it: the pairs, the duplicate's ground there
 * (DATA) and the target's (SHARED), each in the file's order, pieces that go
 * on from one another on the medium joined. */
typedef struct {
    ConjugatePair* pair;
    uint32_t       npair;
    uint32_t       cappair;
    Strip*         dup;
    uint32_t       ndup;
    uint32_t       capdup;
    Strip*         onto;
    uint32_t       nonto;
    uint32_t       caponto;
} ConjugatePlan;

/* A record's runs walked from a block: the Strip under it and the blocks of
 * that Strip behind it. */
typedef struct {
    StripWalk walk;
    Strip     run;
    uint64_t  off;
} ConjugateCursor;

/* What the act holds while it works: the two records as it compared them,
 * where each sat, the walk of the range and the block a refusal met. */
typedef struct {
    const ConjugateAsk* ask;
    FileRecord*         dup;
    RecordWhere         was;
    FileRecord*         onto;
    RecordWhere         there;
    ConjugatePlan       plan;
    uint64_t            at;
    error_t             rc;
} ConjugateAct;

#ifdef CONFIG_VOLUME_TESTS
static void (*volatile s_between)(void* ctx);
static void* volatile s_between_ctx;

void ConjugateStandBetween(void (*between)(void* ctx), void* ctx)
{
    __atomic_store_n(&s_between_ctx, ctx, __ATOMIC_RELEASE);
    __atomic_store_n(&s_between, between, __ATOMIC_RELEASE);
}

static void ConjugateBetween(void)
{
    void (*between)(void* ctx) = __atomic_exchange_n(&s_between, NULL, __ATOMIC_ACQ_REL);
    if (between)
        between(__atomic_load_n(&s_between_ctx, __ATOMIC_ACQUIRE));
}
#endif

// ----------------------------------------------------------------------------
// The walk of the range
// ----------------------------------------------------------------------------

/* Room for one more of `size` bytes in *list, `n` held: false with no memory,
 * or past what a u32 counts. */
static bool ConjugateRoom(void** list, uint32_t n, uint32_t* cap, size_t size)
{
    if (n < *cap)
        return true;
    uint64_t want = *cap ? (uint64_t)*cap * 2u : 8u;
    want          = want > UINT32_MAX ? UINT32_MAX : want;
    void* more    = want > n ? kmalloc(size * want) : NULL;
    if (!more)
        return false;
    if (n)
        memcpy(more, *list, size * n);
    kfree(*list);
    *list = more;
    *cap  = (uint32_t)want;
    return true;
}

/* A piece of ground after the list's last, joined to it where it goes on from
 * it on the medium. */
static bool ConjugateStripPut(Strip** list, uint32_t* n, uint32_t* cap, uint64_t start, uint32_t count,
                              uint32_t kind)
{
    Strip* last = *n ? &(*list)[*n - 1u] : NULL;
    if (last && last->start + last->count == start && last->count <= UINT32_MAX - count) {
        last->count += count;
        return true;
    }
    if (!ConjugateRoom((void**)list, *n, cap, sizeof(Strip)))
        return false;
    (*list)[(*n)++] = (Strip){ .start = start, .count = count, .kind = kind };
    return true;
}

/* `count` blocks of the duplicate from `dup` paired with the target's from
 * `onto`, the duplicate's block `at` of its file: put in the plan. */
static bool ConjugatePiece(ConjugatePlan* p, uint64_t dup, uint64_t onto, uint64_t at, uint32_t count)
{
    ConjugatePair* last = p->npair ? &p->pair[p->npair - 1u] : NULL;
    if (last && last->dup + last->count == dup && last->onto + last->count == onto &&
        last->count <= UINT32_MAX - count)
        last->count += count;
    else if (ConjugateRoom((void**)&p->pair, p->npair, &p->cappair, sizeof(ConjugatePair)))
        p->pair[p->npair++] = (ConjugatePair){ .dup = dup, .onto = onto, .at = at, .count = count };
    else
        return false;
    return ConjugateStripPut(&p->dup, &p->ndup, &p->capdup, dup, count, STRIP_DATA) &&
           ConjugateStripPut(&p->onto, &p->nonto, &p->caponto, onto, count, STRIP_SHARED);
}

static void ConjugatePlanLetGo(ConjugatePlan* p)
{
    kfree(p->pair);
    kfree(p->dup);
    kfree(p->onto);
    memset(p, 0, sizeof(*p));
}

/* The cursor at block `k` of `rec`: false past its runs. */
static bool CursorAt(ConjugateCursor* c, const FileRecord* rec, uint64_t k)
{
    uint64_t begins = 0;
    if (!StripWalkAt(rec, k, &c->walk, &begins) || !StripWalkNext(&c->walk, &c->run))
        return false;
    c->off = k - begins;
    return true;
}

/* The cursor `n` blocks on: false past the record's runs. */
static bool CursorOn(ConjugateCursor* c, uint64_t n)
{
    c->off += n;
    if (c->off < c->run.count)
        return true;
    c->off = 0;
    return StripWalkNext(&c->walk, &c->run);
}

/* The range walked where both records hold their runs: LAID; UNSHAPED where
 * the duplicate is not DATA or the target names no ground (*at the
 * duplicate's block); or REFUSED for no memory. */
static ConjugateWhy ConjugatePlanned(ConjugateAct* a)
{
    const ConjugateAsk* ask = a->ask;
    ConjugateCursor     d, o;
    uint64_t            done = 0;
    a->at = ask->at;
    if (!CursorAt(&d, a->dup, ask->at) || !CursorAt(&o, a->onto, ask->onto_at))
        return CONJUGATE_UNSHAPED;
    while (done < ask->count) {
        uint64_t n = d.run.count - d.off;
        n          = n < o.run.count - o.off ? n : o.run.count - o.off;
        n          = n < ask->count - done ? n : ask->count - done;
        a->at      = ask->at + done;
        if (d.run.kind != STRIP_DATA || !StripOnGround(o.run.kind))
            return CONJUGATE_UNSHAPED;
        if (!ConjugatePiece(&a->plan, d.run.start + d.off, o.run.start + o.off, a->at, (uint32_t)n)) {
            a->rc = ERR_NO_MEMORY;
            return CONJUGATE_REFUSED;
        }
        done += n;
        if (done < ask->count && (!CursorOn(&d, n) || !CursorOn(&o, n)))
            return CONJUGATE_UNSHAPED;
    }
    return CONJUGATE_LAID;
}

// ----------------------------------------------------------------------------
// The guards
// ----------------------------------------------------------------------------

/* The guards the records answer before a block is read: KEPT, UNSHAPED (*at
 * the duplicate's block) or LAID. */
static ConjugateWhy ConjugateGuards(TagFSVolume* vol, ConjugateAct* a)
{
    const ConjugateAsk* ask = a->ask;
    if (InkFileKept(a->dup->flags) || ask->fid == vol->book.boot_fid ||
        ask->onto == vol->book.boot_fid || InkRecordTouchesBoot(vol, a->dup))
        return CONJUGATE_KEPT;
    a->at = ask->at;
    if (ask->at > a->dup->blocks || ask->count > a->dup->blocks - ask->at || ask->onto_at > a->onto->blocks ||
        ask->count > a->onto->blocks - ask->onto_at)
        return CONJUGATE_UNSHAPED;
    a->at = ask->at + ask->count - 1u;
    return TailSlipOn(a->dup, a->at) ? CONJUGATE_UNSHAPED : CONJUGATE_LAID;
}

// ----------------------------------------------------------------------------
// The comparison
// ----------------------------------------------------------------------------

/* The digest the book holds for `block` — the Gloss's for fresh data, the
 * Tally's else: LAID; UNDIGESTED where no element names it or its leaf reads
 * in no copy; REFUSED where the leaf could not be read now. */
static ConjugateWhy ConjugateDigestOf(TagFSVolume* vol, uint64_t block, uint64_t* digest, error_t* rc)
{
    error_t refused = OK;
    if (GlossFind(vol, block, digest))
        return CONJUGATE_LAID;
    switch (TallyDigestFind(vol, block, digest, &refused)) {
    case TALLY_DIGEST_NAMED:
        return CONJUGATE_LAID;
    case TALLY_DIGEST_UNREAD:
        *rc = refused == ERR_NO_MEMORY ? ERR_NO_MEMORY : ERR_READ_FAILED;
        return CONJUGATE_REFUSED;
    default:
        return CONJUGATE_UNDIGESTED;
    }
}

/* Every pair held to its digests, no block read: LAID, or the first refusal
 * at the duplicate's block *at. */
static ConjugateWhy ConjugateDigests(TagFSVolume* vol, ConjugateAct* a)
{
    for (uint32_t i = 0; i < a->plan.npair; i++) {
        const ConjugatePair* p = &a->plan.pair[i];
        for (uint32_t k = 0; k < p->count; k++) {
            uint64_t     mine = 0, theirs = 0;
            ConjugateWhy why  = ConjugateDigestOf(vol, p->dup + k, &mine, &a->rc);
            if (why == CONJUGATE_LAID)
                why = ConjugateDigestOf(vol, p->onto + k, &theirs, &a->rc);
            if (why == CONJUGATE_LAID && mine != theirs)
                why = CONJUGATE_DIFFERS;
            if (why != CONJUGATE_LAID) {
                a->at = p->at + k;
                return why;
            }
        }
    }
    return CONJUGATE_LAID;
}

/* `n` blocks of the pair from its block `from`, read on both sides into
 * `mine` and `theirs` and compared: LAID, COLLIDES where the bytes differ,
 * UNDIGESTED where the target's block does not hold its digest, REFUSED for
 * the medium's refusal — at the duplicate's block *at. */
static ConjugateWhy ConjugateRead(TagFSVolume* vol, ConjugateAct* a, const ConjugatePair* p, uint32_t from, uint32_t n, uint8_t* mine,
                                  uint8_t* theirs)
{
    BoxHashContext ctx;
    BoxHashInit(&ctx, vol->state.uuid, 16);
    a->at = p->at + from;
    a->rc = TagFSReadRun(vol, p->dup + from, n, mine);
    if (a->rc == OK)
        a->rc = TagFSReadRun(vol, p->onto + from, n, theirs);
    if (a->rc != OK)
        return CONJUGATE_REFUSED;
    for (uint32_t k = 0; k < n; k++) {
        const uint8_t* b      = theirs + (size_t)k * TAGFS_BLOCK_SIZE;
        uint64_t       digest = 0;
        a->at                 = p->at + from + k;
        if (memcmp(mine + (size_t)k * TAGFS_BLOCK_SIZE, b, TAGFS_BLOCK_SIZE) != 0)
            return CONJUGATE_COLLIDES;
        ConjugateWhy why = ConjugateDigestOf(vol, p->onto + from + k, &digest, &a->rc);
        if (why != CONJUGATE_LAID)
            return why;
        if (BoxHashIntegrity(b, TAGFS_BLOCK_SIZE, &ctx) != digest)
            return CONJUGATE_UNDIGESTED;
    }
    return CONJUGATE_LAID;
}

/* Every pair read and compared byte for byte, as many blocks a command as the
 * seat takes on either side: LAID, or the first refusal. */
static ConjugateWhy ConjugateBytes(TagFSVolume* vol, ConjugateAct* a)
{
    uint32_t     room   = TagFSRunBlocks(vol);
    uint8_t*     mine   = kmalloc((size_t)room * TAGFS_BLOCK_SIZE);
    uint8_t*     theirs = kmalloc((size_t)room * TAGFS_BLOCK_SIZE);
    ConjugateWhy why    = mine && theirs ? CONJUGATE_LAID : CONJUGATE_REFUSED;
    a->rc               = why == CONJUGATE_LAID ? OK : ERR_NO_MEMORY;
    for (uint32_t i = 0; why == CONJUGATE_LAID && i < a->plan.npair; i++) {
        const ConjugatePair* p = &a->plan.pair[i];
        for (uint32_t from = 0; why == CONJUGATE_LAID && from < p->count; from += room) {
            uint32_t n = p->count - from < room ? p->count - from : room;
            why        = ConjugateRead(vol, a, p, from, n, mine, theirs);
        }
    }
    kfree(mine);
    kfree(theirs);
    return why;
}

// ----------------------------------------------------------------------------
// The act
// ----------------------------------------------------------------------------

/* Under the duplicate's Helm: the records held, guarded, walked, compared and
 * handed to Ink. */
static ConjugateWhy ConjugateHeld(TagFSVolume* vol, ConjugateAct* a, ConjugateSaid* out)
{
    TagFSState*  fs  = &vol->state;
    InkReceipt   got = { 0 };
    ConjugateWhy why = CONJUGATE_LAID;
    a->dup           = FileMapRecordHold(fs->files, a->ask->fid, &a->was);
    a->onto          = FileMapRecordHold(fs->files, a->ask->onto, &a->there);
    if (!a->dup || !a->onto) {
        a->rc = ERR_FILE_NOT_FOUND;
        return CONJUGATE_REFUSED;
    }
    why = ConjugateGuards(vol, a);
    if (why == CONJUGATE_LAID)
        why = ConjugatePlanned(a);
    if (why == CONJUGATE_LAID && InkTouchesBoot(vol, a->plan.onto, a->plan.nonto))
        why = CONJUGATE_KEPT;
    if (why == CONJUGATE_LAID)
        why = ConjugateDigests(vol, a);
    if (why == CONJUGATE_LAID)
        why = ConjugateBytes(vol, a);
    if (why != CONJUGATE_LAID)
        return why;
#ifdef CONFIG_VOLUME_TESTS
    ConjugateBetween();
#endif
    InkConjugation c = { .fid = a->ask->fid, .cur = a->dup, .was = a->was, .at = a->ask->at,
                         .count = a->ask->count, .dup = a->plan.dup, .ndup = a->plan.ndup,
                         .onto = a->plan.onto, .nonto = a->plan.nonto, .onto_fid = a->ask->onto,
                         .onto_gen = a->there.gen };
    a->rc = InkConjugate(vol, &c, &got);
    if (a->rc == ERR_BUSY || a->rc == ERR_FILE_NOT_FOUND)
        return CONJUGATE_CHANGED;
    if (a->rc != OK)
        return CONJUGATE_REFUSED;
    out->blocks = a->ask->count;
    return CONJUGATE_LAID;
}

/* The duplicate opened for the act's change and its Helm taken — a holder's
 * write is not waited for: OK, or the refusal. */
static ConjugateWhy ConjugateOpened(TagFSVolume* vol, ConjugateAct* a, ConjugateSaid* out)
{
    TagFSFileHandle* h    = tagfs_open(vol, a->ask->fid, TAGFS_HANDLE_WRITE);
    HelmWaiter       here = { 0 };
    ConjugateWhy     why  = CONJUGATE_REFUSED;
    if (!h) {
        a->rc = BookFileLocked(vol, a->ask->fid) ? ERR_TAGFS_CORRUPTED : ERR_FILE_NOT_FOUND;
        return why;
    }
    h->warrant = INK_WARRANTED;
    a->rc      = HelmTakeNow(vol, h, &here);
    if (a->rc == OK) {
        why = ConjugateHeld(vol, a, out);
        HelmLetGo(vol, h->ofe, &here);
    }
    FileRecordRelease(a->dup);
    FileRecordRelease(a->onto);
    ConjugatePlanLetGo(&a->plan);
    tagfs_close(vol, h);
    return why;
}

/* Whether the ask names a range a file can be laid on: some blocks, as many as
 * a node of Fallow counts runs, none past a u64, and a file laid on itself
 * laid on blocks outside the range. */
static bool ConjugateAskHolds(const ConjugateAsk* ask)
{
    if (ask->count == 0 || ask->count > UINT32_MAX || ask->at > UINT64_MAX - ask->count ||
        ask->onto_at > UINT64_MAX - ask->count)
        return false;
    return ask->fid != ask->onto || ask->at + ask->count <= ask->onto_at || ask->onto_at + ask->count <= ask->at;
}

static error_t ConjugateErrorOf(ConjugateWhy why, error_t rc)
{
    switch (why) {
    case CONJUGATE_LAID:
        return OK;
    case CONJUGATE_UNWARRANTED:
    case CONJUGATE_KEPT:
        return ERR_PERMISSION_DENIED;
    case CONJUGATE_UNSHAPED:
    case CONJUGATE_DIFFERS:
    case CONJUGATE_COLLIDES:
        return ERR_INVALID_ARGUMENT;
    case CONJUGATE_UNDIGESTED:
        return ERR_CHECKSUM;
    case CONJUGATE_CHANGED:
        return ERR_BUSY;
    default:
        return rc;
    }
}

static const char* const s_why[] = {
    [CONJUGATE_UNWARRANTED] = "the cabin asking wears neither system nor god",
    [CONJUGATE_KEPT]        = "the duplicate is a file the system keeps in a role, or one of the two holds the kernel the "
                              "loader reads by its blocks",
    [CONJUGATE_UNSHAPED]    = "the blocks asked are not the duplicate's plain ground or not the target's ground, "
                              "from the duplicate's block",
    [CONJUGATE_UNDIGESTED]  = "the book holds no digest of a block, or the target's does not hold it — the "
                              "duplicate's block",
    [CONJUGATE_DIFFERS]     = "the digests differ at the duplicate's block",
    [CONJUGATE_COLLIDES]    = "the digests agree and the bytes differ at the duplicate's block",
    [CONJUGATE_CHANGED]     = "a file changed between the comparison and the Ink",
    [CONJUGATE_REFUSED]     = "the volume refused it",
};

/* The act said, laid or refused — but for the ribbon's room on its way, no
 * answer: the caller waits for the cycle's end and asks again
 * (settle_ops.c, SettleOpForRoom), and the ribbon says its own wait. */
static void ConjugateSays(const ConjugateAsk* ask, const ConjugateSaid* said, error_t rc)
{
    char block[24] = "";
    if (rc == ERR_WOULD_BLOCK)
        return;
    if (said->why == CONJUGATE_UNSHAPED || said->why == CONJUGATE_UNDIGESTED || said->why == CONJUGATE_DIFFERS ||
        said->why == CONJUGATE_COLLIDES)
        ksnprintf(block, sizeof(block), " %llu", (unsigned long long)said->at);
    if (said->why == CONJUGATE_LAID) {
        kprintf("[TagFS] Conjugate: fid %llu's %llu block(s) from its block %llu are laid on fid %llu's from its "
                "block %llu — its own rest in Fallow\n", (unsigned long long)ask->fid,
                (unsigned long long)ask->count, (unsigned long long)ask->at, (unsigned long long)ask->onto,
                (unsigned long long)ask->onto_at);
        return;
    }
    kprintf("[TagFS] Conjugate of fid %llu's %llu block(s) from its block %llu onto fid %llu's from its block %llu "
            "refused (error %u): %s%s\n", (unsigned long long)ask->fid, (unsigned long long)ask->count,
            (unsigned long long)ask->at, (unsigned long long)ask->onto, (unsigned long long)ask->onto_at,
            (unsigned)rc, s_why[said->why], block);
}

/* The act, asked with `warrant`, said by its caller. */
static error_t ConjugateAsked(TagFSVolume* vol, InkWarrant warrant, const ConjugateAsk* ask, ConjugateSaid* out)
{
    ConjugateAct a   = { .ask = ask, .at = ask->at, .rc = OK };
    ConjugateWhy why = CONJUGATE_REFUSED;
    memset(out, 0, sizeof(*out));
    if (warrant != INK_WARRANTED)
        why = CONJUGATE_UNWARRANTED;
    else if (!ConjugateAskHolds(ask))
        why = CONJUGATE_UNSHAPED;
    else if (!tagfs_enter(vol))
        a.rc = ERR_DEVICE_NOT_READY;
    else {
        a.rc = InkVolumeBegins(vol);
        if (a.rc == OK)
            why = ConjugateOpened(vol, &a, out);
        tagfs_leave(vol);
    }
    out->why = why;
    out->at  = a.at;
    return ConjugateErrorOf(why, a.rc);
}

error_t TagFSConjugate(TagFSVolume* vol, InkWarrant warrant, const ConjugateAsk* ask, ConjugateSaid* out)
{
    error_t rc = ConjugateAsked(vol, warrant, ask, out);
    ConjugateSays(ask, out, rc);
    return rc;
}

error_t ConjugateLay(TagFSVolume* vol, const ConjugateAsk* ask, ConjugateSaid* out)
{
    return ConjugateAsked(vol, INK_WARRANTED, ask, out);
}
