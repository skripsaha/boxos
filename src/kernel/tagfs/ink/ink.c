#include "ink_internal.h"
#include "../mount/book_mount.h"
#include "../settle/sewing.h"
#include "../helm/engross.h"
#include "boardroom.h"
#include "atomics.h"
#include "cpu_calibrate.h"

/*
 * The halves of every Ink (ink.h): the record built with no lock held, then
 * laid, named and indexed under the volume's state lock with nothing left that
 * can refuse. The counters below move under that lock alone.
 */

static uint64_t s_inked_gen;     /* the newest Ink's */
static uint64_t s_data_need;     /* the flush need the newest data an Ink named holds */
static volatile uint8_t s_read_only;   /* the medium refused a write as protected */

uint64_t InkedGen(void)
{
    return __atomic_load_n(&s_inked_gen, __ATOMIC_ACQUIRE);
}

uint64_t InkDataNeed(void)
{
    return s_data_need;
}

uint64_t InkGenNextLocked(void)
{
    return s_inked_gen + 1u;
}

void InkGenLaidLocked(uint64_t gen)
{
    __atomic_store_n(&s_inked_gen, gen, __ATOMIC_RELEASE);
}

void InkDataNeedRaiseLocked(uint64_t need)
{
    if (need > s_data_need)
        s_data_need = need;
}

/*
 * ‼ WHAT THE RIBBON LET GO RESTS AT A MOUNT, NAMED BY THE LAST STITCH READ
 * (M-1, §3.4, §16.2 п.16). The ground a book's records gave up, as the census
 * left it — what no other owner names (book_ground.c) — is neither a file's
 * nor free: what the mount read may have come out of the medium's cache after
 * a warm reboot, and on a USB disk with no SYNC CACHE nothing else closes that
 * window. So it comes to rest as one record freer's ground, named by the last
 * stitch the ribbon holds as this mount's cycle 0 — it goes back once a stitch
 * of this mount leaves the Quire the ribbon stands in and the lag's stitches
 * of this mount settled. With no memory to hold it, it stays out of the
 * allocator until the next mount, said.
 */
static void InkMountFallow(void)
{
    const RibbonCursor* c    = RibbonCursorNow();
    uint64_t            left = g_book.nfallow;
    if (!c->up)
        return;
    for (const Strip* runs = g_book.fallow; left;) {
        uint32_t    n    = left > UINT32_MAX ? UINT32_MAX : (uint32_t)left;
        FallowNode* node = FallowNodeMake(runs, n, FALLOW_BY_RECORD, 0);
        if (!node) {
            kprintf("[TagFS] there is no memory to rest the ground the ribbon let go in Fallow: %llu run(s) of it "
                    "stay out of the allocator until the volume is mounted again\n", (unsigned long long)left);
            return;
        }
        TagFSLockTake(&InkState()->lock);
        FallowAdd(node, 0);
        FallowStitchSettled(0, c->last, 0);
        TagFSLockDrop(&InkState()->lock);
        runs += n;
        left -= n;
    }
}

void InkMountReset(void)
{
    __atomic_store_n(&s_inked_gen, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_read_only, 0u, __ATOMIC_RELEASE);
    s_data_need = 0;
    DocketDropAll();
    if (!TagFSIsUnderGlass())
        InkMountFallow();
}

void InkDropAll(void)
{
    FallowDropAll();
    DocketDropAll();
}

bool TagFSReadOnly(void)
{
    return __atomic_load_n(&s_read_only, __ATOMIC_ACQUIRE) != 0 || TagFSIsUnderGlass() || StitchBookRefused();
}

void InkReadOnlyLift(void)
{
    __atomic_store_n(&s_read_only, 0u, __ATOMIC_RELEASE);
}

/* Asked after any write the medium refused: a refusal the seat calls
 * protection is final for this seating of the medium, and said once. */
void TagFSWriteRefused(void)
{
    if (!BoardroomSeatRefusesWrites(tagfs_get_seat()))
        return;
    if (__atomic_exchange_n(&s_read_only, 1u, __ATOMIC_ACQ_REL))
        return;
    kprintf("[TagFS] the medium under the volume refuses writes as protected — the volume "
            "takes no change from here on (ERR_TAGFS_READ_ONLY), and %llu change(s) no cycle "
            "settled stay unwritten\n",
            (unsigned long long)(InkedGen() - SettledGen()));
    SettleKnock();
}

uint64_t InkGenOf(uint64_t fid)
{
    RecordWhere where;
    FileRecord* rec = FileMapRecordHold(InkState()->files, fid, &where);
    if (!rec)
        return InkedGen();
    FileRecordRelease(rec);
    return where.gen;
}

// ----------------------------------------------------------------------------
// The guards (RT-09, RT-13)
// ----------------------------------------------------------------------------

/* The kernel's blocks as a record names them — [*first, *end) — or false when
 * the Deed points the loader at none of them. */
static bool InkBootSpan(uint64_t* first, uint64_t* end)
{
    TagFSState* fs = InkState();
    if (!fs->boot.kernel_blocks)
        return false;
    *first = fs->boot.kernel_block;
    *end   = *first + fs->boot.kernel_blocks;
    return true;
}

bool InkTouchesBoot(const Strip* runs, uint32_t count)
{
    uint64_t first = 0, end = 0;
    if (!InkBootSpan(&first, &end))
        return false;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t a = runs[i].start, b = a + runs[i].count;
        if (StripOnGround(runs[i].kind) && a < end && b > first)    /* a hole lies on no block */
            return true;
    }
    return false;
}

bool InkRecordTouchesBoot(const FileRecord* rec)
{
    uint64_t  first = 0, end = 0;
    StripWalk walk;
    Strip     run;
    if (!InkBootSpan(&first, &end))
        return false;
    StripWalkBegin(rec, &walk);
    while (StripWalkNextGround(&walk, &run))
        if (run.start < end && run.start + run.count > first)
            return true;
    return false;
}

/* Whether the volume takes a change at all: asked in the first half, and again
 * under the state lock where the Ink is laid, so an Ink built while the medium
 * left or refused a write as protected is not laid after it (RT-13). */
error_t InkVolumeTakes(void)
{
    if (TagFSMediumLeft())
        return ERR_DEVICE_NOT_READY;
    if (TagFSReadOnly())
        return ERR_TAGFS_READ_ONLY;
    return OK;
}

/* Whether the volume takes a change not begun yet: none once a stopping
 * machine's halt shut it (TagFSShutForHalt). Asked in the first half alone — a
 * change begun before the shut is the halt's to settle, and a write's bytes may
 * already stand in place. The mark a mount's verdict owes its files is made by
 * the cycle itself before its snapshot, on the core that holds it — on that
 * cycle's books, a halt's too (sewing.c, StitchMarked). */
error_t InkVolumeBegins(void)
{
    if (SettleHalting() && !StitchMarksVerdict())
        return ERR_DEVICE_NOT_READY;
    return InkVolumeTakes();
}

/* The ribbon is asked too whether the Coineds of the tags a change brings
 * find room on it now, and they are held there (RibbonReserve) — or,
 * bringing none new, whether a record would (RibbonClaim, nothing claimed): a
 * create, a name or a tag refused there interns nothing for the next stitch to
 * lay down as a Coined, and every tag interned has its Coined's room. */
error_t InkBookRefuses(const TagSpelling* tags, uint32_t count, uint64_t* held)
{
    TagFSState* fs    = InkState();
    uint64_t    bytes = 0;
    *held = 0;
    error_t rc = InkVolumeBegins();
    if (rc != OK)
        return rc;
    for (uint32_t i = 0; i < count; i++)
        if (tags[i].key && TagRegistryLookup(fs->registry, &tags[i]) == TAGFS_INVALID_TAG_ID)
            bytes += STITCH_COINED_MOST + tags[i].key_len + tags[i].value_len;
    TagFSLockTake(&fs->lock);
    rc = bytes ? RibbonReserve(bytes) : RibbonClaim(0, false);
    TagFSLockDrop(&fs->lock);
    rc = RibbonClaimSay(rc);
    if (rc == OK)
        *held = bytes;
    return rc;
}

void InkBookHeldLetGo(uint64_t held)
{
    if (!held)
        return;
    TagFSLockTake(&InkState()->lock);
    RibbonReserveLetGo(held);
    TagFSLockDrop(&InkState()->lock);
}

/* The guard's answer, `boot` the answer to whether the change touches the
 * kernel's blocks: those are refused to every call; a file the system keeps
 * in a role (its record's flags, never its tags), to a call with no warrant. */
error_t InkGuardSays(uint64_t fid, bool boot, uint32_t flags, InkWarrant warrant)
{
    error_t rc = InkVolumeTakes();
    if (rc != OK)
        return rc;
    if (boot || (warrant != INK_WARRANTED && InkFileKept(flags))) {
        if (boot)
            kprintf("[TagFS] file %llu holds the kernel the Deed points the loader at — it is "
                    "changed by no call, warrant or none\n", fid);
        else
            kprintf("[TagFS] file %llu is the system's — a system, boot or kernel file in its "
                    "record — and the call that would change it carries no warrant — it is not "
                    "changed\n", fid);
        return ERR_PERMISSION_DENIED;
    }
    return OK;
}

/* The guard with the ground a change lets go of asked too: a Plot's freed runs
 * are the kernel's no more than its runs may be. A Plot holds a window of its
 * file's runs, not all of them: the file the loader reads the kernel by is
 * known by its id, named when the mount laid every file's ground
 * (book_ground.c, BookFilesLaid). */
static error_t InkGuard(uint64_t fid, uint32_t flags, const Strip* runs, uint32_t run_count,
                        const Strip* freed, uint32_t freed_count, InkWarrant warrant)
{
    bool boot = fid == g_book.boot_fid ||
                InkTouchesBoot(runs, run_count) || InkTouchesBoot(freed, freed_count);
    return InkGuardSays(fid, boot, flags, warrant);
}

error_t InkMayChange(uint64_t fid, uint32_t flags, const Strip* runs, uint32_t run_count,
                     InkWarrant warrant)
{
    error_t rc = InkVolumeBegins();
    if (rc != OK)
        return rc;
    if (flags & TAGFS_FILE_DELETED)
        return ERR_FILE_NOT_FOUND;
    return InkGuard(fid, flags, runs, run_count, NULL, 0, warrant);
}

// ----------------------------------------------------------------------------
// An answer that is not final
// ----------------------------------------------------------------------------

/* What the loops below do with an answer that is not final: INK_AGAIN goes
 * round again. ERR_WOULD_BLOCK is the caller's to wait on as it comes: room a
 * Settle cycle gives back, which its caller waits for its own way (helm.h,
 * settle_ops.h) — no cycle is run beneath an Ink. */
bool InkGoesRound(const error_t* rc)
{
    return *rc == INK_AGAIN;
}

// ----------------------------------------------------------------------------
// A Plot's size and extents
// ----------------------------------------------------------------------------

/*
 * Whether a write is a frequent small writer's (settle.h), told by what it
 * does: it grew the file by data that cost the medium less time than the last
 * Settle cycle did — settling it would cost more than it — and the file's last
 * change was such an append too, less than a while ago: the window itself
 * (§3.7, В5 — such a writer's cycles come at most one a window). The measure
 * is the medium's own, measured; nothing here is a number of bytes or of
 * writes. `small` says whether this one is
 * such an append, for the next to ask.
 */
static bool InkPlotLazy(const Plot* p, const FileRecord* cur, uint64_t now_us, bool* small)
{
    uint64_t d = SettleLastDUs();
    *small = p->size > p->size_before && d != 0 && p->data_us < d;
    return *small && cur->small_append_us != 0 && now_us > cur->small_append_us &&
           now_us - cur->small_append_us < SETTLE_WINDOW_US;
}

/* The flags the Plot was loaded with are the ones asked, with the warrant it
 * was loaded with: a file that came to be kept in a role after the load had
 * its bytes laid already, and refusing the record now would take back its
 * size, not its bytes. */
error_t TagFSInkPlot(Plot* p, InkReceipt* out)
{
    error_t rc = InkGuard(p->fid, p->meta.flags, p->runs, p->run_count,
                          p->freed, p->freed_count, p->warrant);
    if (rc != OK)
        return rc;

    /* The Ink makes its Fallow node of the ground its record stops naming
     * itself (ink_book.c, InkBookPlotBuilt). */
    uint64_t now_us = cpu_tsc_to_us(rdtsc());
    bool     lazy   = false;
    for (;;) {
        RecordWhere was;
        FileRecord* cur = FileMapRecordHold(InkState()->files, p->fid, &was);
        if (!cur) {
            rc = ERR_FILE_NOT_FOUND;
            break;
        }
        if (was.shape != p->shape) {
            FileRecordRelease(cur);
            kprintf("[TagFS] file %llu: its extents were changed since this call loaded them, by a "
                    "caller that did not hold its Helm — this record is refused rather than laid "
                    "over that one\n", p->fid);
            rc = ERR_BUSY;
            break;
        }
        bool small = false;
        lazy = InkPlotLazy(p, cur, now_us, &small);
        rc   = InkBookPlot(p, cur, &was, small ? now_us : 0, out);
        FileRecordRelease(cur);
        if (rc == OK || !InkGoesRound(&rc))
            break;
    }
    if (rc != OK)
        return rc;

    p->stake_count   = 0;
    memset(&p->stakes_counted, 0, sizeof(p->stakes_counted));   /* let go with its Ink (ink_book.c) */
    p->freed_count   = 0;
    p->size_before   = p->size;
    p->blocks_before = p->blocks;
    p->shape         = out->gen;
    p->data_need     = 0;
    p->data_us       = 0;
    InkBookPlotNext(p);
    SettleInked(out->gen, lazy);
    return OK;
}

// ----------------------------------------------------------------------------
// A file's name and tags
// ----------------------------------------------------------------------------

/* The tags the file wears after the change, into a list of its own; false
 * for no memory. */
static bool InkFieldsTags(const FileRecord* cur, const char* name, uint32_t add, uint32_t drop,
                          InkFieldsPlan* f)
{
    uint32_t old_name = name ? InkNameTagLookup(cur->name) : TAGFS_INVALID_TAG_ID;
    memset(f, 0, sizeof(*f));
    f->tags = kmalloc(sizeof(uint32_t) * (cur->tag_count + 1u));
    if (!f->tags)
        return false;

    bool worn = false;
    if (name && add != TAGFS_INVALID_TAG_ID)
        f->tags[f->count++] = add;
    for (uint32_t i = 0; i < cur->tag_count; i++) {
        uint32_t t = cur->tags[i];
        if (t == add) {
            worn = true;
            if (name)
                continue;               /* it went first already */
        }
        if ((t == drop || (t == old_name && t != add)) && t != TAGFS_INVALID_TAG_ID) {
            f->dropped[f->drops++ & 1u] = t;
            continue;
        }
        f->tags[f->count++] = t;
    }
    if (!name && add != TAGFS_INVALID_TAG_ID && !worn)
        f->tags[f->count++] = add;
    f->adds = add != TAGFS_INVALID_TAG_ID && !worn;
    return true;
}

/* The role InkFieldsOnce leaves a record in when it is asked for none. */
#define INK_ROLE_AS_IS UINT32_MAX

/* One round of InkFields: OK, a final refusal, or INK_AGAIN / ERR_WOULD_BLOCK.
 * `role` is the one the record is to be kept in, or INK_ROLE_AS_IS. */
static error_t InkFieldsOnce(uint64_t fid, const char* name, uint32_t add, uint32_t drop,
                             uint32_t role, InkReceipt* out)
{
    TagFSState* fs = InkState();
    RecordWhere was;
    FileRecord* cur = FileMapRecordHold(fs->files, fid, &was);
    if (!cur)
        return ERR_FILE_NOT_FOUND;

    InkFieldsPlan f;
    error_t       rc = InkVolumeBegins();
    if (rc == OK && (cur->flags & TAGFS_FILE_DELETED))
        rc = ERR_FILE_NOT_FOUND;
    if (!InkFieldsTags(cur, name, add, drop, &f)) {
        FileRecordRelease(cur);
        return ERR_NO_MEMORY;
    }
    uint32_t flags = role == INK_ROLE_AS_IS ? cur->flags : (cur->flags & ~TAGFS_FILE_ROLE_MASK) | role;
    if (rc == OK && !name && !f.adds && f.drops == 0 && flags == cur->flags) {
        rc       = drop != TAGFS_INVALID_TAG_ID ? ERR_TAGFS_TAG_NOT_FOUND : OK;
        out->gen = was.gen;
    } else if (rc == OK) {
        rc = InkBookFields(fid, cur, &was, &f, name ? name : cur->name, add, flags, cur->keep_until, out);
    }
    kfree(f.tags);
    FileRecordRelease(cur);
    return rc;
}

error_t InkFields(uint64_t fid, const char* name, uint32_t add, uint32_t drop, InkReceipt* out)
{
    error_t rc;
    do {
        rc = InkFieldsOnce(fid, name, add, drop, INK_ROLE_AS_IS, out);
    } while (rc != OK && InkGoesRound(&rc));
    return rc;
}

#ifdef CONFIG_VOLUME_TESTS
error_t InkRoleLaid(uint64_t fid, uint32_t role, InkReceipt* out)
{
    error_t rc;
    do {
        rc = InkFieldsOnce(fid, NULL, TAGFS_INVALID_TAG_ID, TAGFS_INVALID_TAG_ID, role, out);
    } while (rc != OK && InkGoesRound(&rc));
    return rc;
}
#endif

static error_t InkUndoneOnce(uint64_t fid, InkReceipt* out)
{
    RecordWhere   was;
    InkFieldsPlan f = { 0 };
    FileRecord*   cur = FileMapRecordHold(InkState()->files, fid, &was);
    if (!cur)
        return ERR_FILE_NOT_FOUND;
    error_t rc = InkVolumeBegins();
    if (rc == OK && (cur->flags & TAGFS_FILE_UNDONE))
        out->gen = was.gen;
    else if (rc == OK && !InkFieldsTags(cur, NULL, TAGFS_INVALID_TAG_ID, TAGFS_INVALID_TAG_ID, &f))
        rc = ERR_NO_MEMORY;
    else if (rc == OK)
        rc = InkBookFields(fid, cur, &was, &f, cur->name, TAGFS_INVALID_TAG_ID, cur->flags | TAGFS_FILE_UNDONE,
                           cur->keep_until, out);
    kfree(f.tags);
    FileRecordRelease(cur);
    return rc;
}

error_t InkUndone(uint64_t fid, InkReceipt* out)
{
    error_t rc;
    do {
        rc = InkUndoneOnce(fid, out);
    } while (rc != OK && InkGoesRound(&rc));
    return rc;
}

/*
 * One round of InkKeep: the file's record made the deleted one kept until
 * `until` — or, for 0, made the one of a file that is not. The tags are the
 * ones it wears; the tag index's set of the deleted files takes the file in or
 * lets it go in the same locked step as the record (InkBookFieldsLocked, a tag
 * added or dropped). A call that changes nothing is OK with no Ink.
 */
static error_t InkKeepOnce(uint64_t fid, uint64_t until, InkWarrant warrant, InkReceipt* out)
{
    RecordWhere   was;
    InkFieldsPlan f = { 0 };
    FileRecord*   cur = FileMapRecordHold(InkState()->files, fid, &was);
    if (!cur)
        return ERR_FILE_NOT_FOUND;
    bool     kept  = cur->flags & TAGFS_FILE_DELETED;
    uint32_t flags = until ? cur->flags | TAGFS_FILE_DELETED : cur->flags & ~(uint32_t)TAGFS_FILE_DELETED;
    error_t  rc    = InkVolumeBegins();
    if (rc == OK)
        rc = InkGuardSays(fid, InkRecordTouchesBoot(cur), cur->flags, warrant);
    if (rc != OK)
        goto out;
    if (kept == (until != 0) && cur->keep_until == until) {
        out->gen = was.gen;
        goto out;
    }
    if (!InkFieldsTags(cur, NULL, TAGFS_INVALID_TAG_ID, TAGFS_INVALID_TAG_ID, &f)) {
        rc = ERR_NO_MEMORY;
        goto out;
    }
    if (until && !kept)
        f.adds = true;
    else if (!until)
        f.dropped[f.drops++] = TAGFS_DELETED_MEMBERS;
    rc = InkBookFields(fid, cur, &was, &f, cur->name, TAGFS_DELETED_MEMBERS, flags, until, out);
out:
    kfree(f.tags);
    FileRecordRelease(cur);
    return rc;
}

error_t InkKeep(uint64_t fid, uint64_t until, InkWarrant warrant, InkReceipt* out)
{
    error_t rc;
    do {
        rc = InkKeepOnce(fid, until, warrant, out);
    } while (rc != OK && InkGoesRound(&rc));
    return rc;
}

// ----------------------------------------------------------------------------
// A new file
// ----------------------------------------------------------------------------

error_t InkNew(const char* name, const uint32_t* tags, uint32_t count, uint64_t made_by, uint64_t* out_fid,
               InkReceipt* out)
{
    error_t rc = InkVolumeBegins();
    if (rc != OK)
        return rc;
    return InkBookNew(name, tags, count, made_by, out_fid, out);
}

// ----------------------------------------------------------------------------
// A file given up
// ----------------------------------------------------------------------------

/* One round of InkQuitclaim. */
static error_t InkQuitclaimOnce(uint64_t fid, InkWarrant warrant, InkReceipt* out)
{
    TagFSState* fs = InkState();
    RecordWhere was;
    FileRecord* cur = FileMapRecordHold(fs->files, fid, &was);
    if (!cur)
        return ERR_FILE_NOT_FOUND;

    /* Guarded on the record where it holds its runs, with no allocation: a
     * file the system keeps, or the kernel's, is refused before the heap is
     * asked for anything, and a heap with nothing left turns neither refusal
     * into another. The ground let go is walked the same way. */
    FallowNode* fallow = NULL;
    error_t     rc     = InkVolumeBegins();
    if (rc == OK)
        rc = InkGuardSays(fid, InkRecordTouchesBoot(cur), cur->flags, warrant);
    if (rc == OK)
        rc = FallowNodeOfRecord(cur, FALLOW_BY_RECORD, &fallow);
    if (rc == OK)
        rc = InkBookQuitclaim(fid, &was, cur, fallow, out);
    FileRecordRelease(cur);
    return rc;
}

/* A holder of the file's Helm is not waited for: the file is vacated, and the
 * holder answers ERR_FILE_NOT_FOUND at its own Ink, its Stakes given back. */
error_t InkQuitclaim(uint64_t fid, InkWarrant warrant, InkReceipt* out)
{
    error_t rc;
    do {
        rc = InkQuitclaimOnce(fid, warrant, out);
    } while (rc != OK && InkGoesRound(&rc));
    if (rc == OK)
        HelmVacate(fid);
    return rc;
}
