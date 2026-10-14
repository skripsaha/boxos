#include "ink.h"
#include "fallow.h"
#include "../metadata_pool/meta_pool.h"
#include "../file_map/file_map.h"
#include "../file_table/file_table.h"
#include "../tag_registry/tag_registry.h"
#include "../settle/settle.h"
#include "../helm/helm.h"
#include "../glean/glean.h"
#include "../../../kernel/drivers/timer/rtc.h"
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

/* A retry of the first half: the file changed between the halves, or a stake
 * no longer covers. Never an answer. */
#define INK_AGAIN ((error_t)0xFFFFFFFFu)

uint64_t InkedGen(void)
{
    return __atomic_load_n(&s_inked_gen, __ATOMIC_ACQUIRE);
}

uint64_t InkDataNeed(void)
{
    return s_data_need;
}

void InkMountReset(void)
{
    __atomic_store_n(&s_inked_gen, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_read_only, 0u, __ATOMIC_RELEASE);
    s_data_need = 0;
}

bool TagFSReadOnly(void)
{
    return __atomic_load_n(&s_read_only, __ATOMIC_ACQUIRE) != 0 || TagFSIsUnderGlass();
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

static TagFSState* InkState(void)
{
    return tagfs_get_state();
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

/* Whether a tag's key is one the system keeps: system, boot, god — by the key,
 * so system:foo is the system's as much as system is. */
static bool InkTagGuarded(uint32_t tag_id)
{
    TagSpelling tag = { 0 };
    if (tag_id == TAGFS_INVALID_TAG_ID ||
        !TagRegistrySpelling(InkState()->registry, tag_id, &tag, NULL))
        return false;
    return TagSpanIs("system", tag.key, tag.key_len) || TagSpanIs("boot", tag.key, tag.key_len) ||
           TagSpanIs("god", tag.key, tag.key_len);
}

/* The kernel's blocks as a record names them — [*first, *end) — or false when
 * the Deed points the loader at none of them. */
static bool InkBootSpan(uint64_t* first, uint64_t* end)
{
    TagFSState* fs = InkState();
    if (!fs->boot.kernel_blocks || fs->boot.kernel_block < fs->block_base)
        return false;
    *first = fs->boot.kernel_block - fs->block_base;
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
        if (runs[i].kind == STRIP_DATA && a < end && b > first)    /* a hole lies on no block */
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

bool InkWearsGuarded(const uint32_t* tags, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
        if (InkTagGuarded(tags[i]))
            return true;
    return false;
}

/* Whether the volume takes a change at all: asked in the first half, and again
 * under the state lock where the Ink is laid, so an Ink built while the medium
 * left or refused a write as protected is not laid after it (RT-13). */
static error_t InkVolumeTakes(void)
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
 * already stand in place. */
static error_t InkVolumeBegins(void)
{
    if (SettleHalting())
        return ERR_DEVICE_NOT_READY;
    return InkVolumeTakes();
}

/* The guard's answer, `boot` the answer to whether the change touches the
 * kernel's blocks: those are refused to every call; a file the system keeps,
 * to a call with no warrant. */
static error_t InkGuardSays(uint64_t fid, bool boot, const uint32_t* tags, uint32_t tag_count,
                            InkWarrant warrant)
{
    error_t rc = InkVolumeTakes();
    if (rc != OK)
        return rc;
    if (boot || (warrant != INK_WARRANTED && InkWearsGuarded(tags, tag_count))) {
        if (boot)
            kprintf("[TagFS] file %llu holds the kernel the Deed points the loader at — it is "
                    "changed by no call, warrant or none\n", fid);
        else
            kprintf("[TagFS] file %llu wears system, boot or god, and the call that would change "
                    "it carries no warrant — it is not changed\n", fid);
        return ERR_PERMISSION_DENIED;
    }
    return OK;
}

/* The guard with the ground a change lets go of asked too: a Plot's freed runs
 * are the kernel's no more than its runs may be. */
static error_t InkGuard(uint64_t fid, const uint32_t* tags, uint32_t tag_count,
                        const Strip* runs, uint32_t run_count,
                        const Strip* freed, uint32_t freed_count, InkWarrant warrant)
{
    bool boot = InkTouchesBoot(runs, run_count) || InkTouchesBoot(freed, freed_count);
    return InkGuardSays(fid, boot, tags, tag_count, warrant);
}

error_t InkMayChange(uint64_t fid, const uint32_t* tags, uint32_t tag_count,
                     const Strip* runs, uint32_t run_count, InkWarrant warrant)
{
    error_t rc = InkVolumeBegins();
    if (rc != OK)
        return rc;
    return InkGuard(fid, tags, tag_count, runs, run_count, NULL, 0, warrant);
}

// ----------------------------------------------------------------------------
// The record, built before the lock
// ----------------------------------------------------------------------------

typedef struct {
    FileRecord* rec;        /* memory's copy; NULL for a Quitclaim */
    uint8_t*    packed;     /* the medium's */
    uint32_t    bytes;
} InkRecord;

static void InkRecordLetGo(InkRecord* r)
{
    FileRecordRelease(r->rec);
    kfree(r->packed);
    r->rec    = NULL;
    r->packed = NULL;
}

/* `meta` packed into the medium's copy of the record — memory's is the
 * caller's to build beside it (FileRecordBuild, Relabel, Splice): OK,
 * ERR_TAGFS_RECORD_FULL for a record wider than format v1 holds, PackV1's
 * refusal of what one cannot carry, ERR_NO_MEMORY. */
static error_t InkRecordMake(const TagFSMetadata* meta, InkRecord* r)
{
    memset(r, 0, sizeof(*r));
    if (!PlotFits(meta, &r->bytes))
        return ERR_TAGFS_RECORD_FULL;
    r->packed = kmalloc(r->bytes);
    if (!r->packed)
        return ERR_NO_MEMORY;
    error_t packed = PackV1(meta, TagFSLayout()->data_blocks, r->packed, &r->bytes);
    if (packed != OK)
        InkRecordLetGo(r);
    return packed;
}

// ----------------------------------------------------------------------------
// The tail, under the lock
// ----------------------------------------------------------------------------

/* What a change may take of the pool's room (meta_pool.h, the reserve). */
typedef enum {
    INK_FREES_NOTHING = 0,     /* a record: a turn past the whole reserve */
    INK_GIVES_UP_NOTHING,      /* a Quitclaim of a file with no ground: past Glean's share */
    INK_FREES_GROUND,          /* a change that lets ground go: past the close it needs */
} InkFrees;

typedef struct {
    bool     back;          /* the file's record in the tail is taken back first */
    uint32_t back_len;
    uint32_t keep;          /* the Spares a turn leaves the pool */
} InkPlan;

/*
 * Room for `bytes`, planned and nothing changed: ERR_WOULD_BLOCK when the tail
 * has too little and no Spare past the change's floor may be had, or room the
 * change may not take. While the volume has a free block, the floor is the
 * whole reserve — the caller makes a Spare of its own from it; with none, the
 * change's own (meta_pool.h). And below Glean's share no change takes the
 * tail's room unless what it leaves covers what comes after it: the Spare of
 * the close that lets what the tail holds go, at least, and for a change that
 * frees nothing the step Glean takes next (GleanStepNeedLocked), `was` being
 * the record it kills — on a volume with no free block, the room is Glean's to
 * make.
 */
static error_t InkPlanFor(uint64_t fid, const RecordWhere* was, uint32_t bytes, InkFrees frees,
                          InkPlan* plan)
{
    uint32_t floor  = frees == INK_FREES_NOTHING    ? (uint32_t)SPARE_RESERVE
                    : frees == INK_GIVES_UP_NOTHING ? (uint32_t)SPARE_FOR_GLEAN
                                                    : 1u;   /* the close its ground needs */
    plan->back      = was && PoolTailHolds(was, fid, &plan->back_len);
    plan->keep      = InkState()->ledger.free_blocks ? (uint32_t)SPARE_RESERVE : floor;
    uint32_t room   = PoolTailRoom() + (plan->back ? plan->back_len : 0);
    uint32_t spares = PoolSparesHeld(NULL, 0);
    if (room < bytes)
        return spares > plan->keep ? OK : ERR_WOULD_BLOCK;
    if (spares >= (uint32_t)SPARE_FOR_GLEAN)
        return OK;
    uint32_t need = frees == INK_FREES_GROUND ? 0u : GleanStepNeedLocked(room - bytes, was);
    return spares >= (need > 1u ? need : 1u) ? OK : ERR_WOULD_BLOCK;
}

/* The plan carried out and the record laid: where it sits. From here nothing
 * refuses. What the replaced record had freed is freed by this Ink now. */
static RecordWhere InkLayDown(const RecordWhere* was, const InkPlan* plan, const InkRecord* r,
                              uint64_t gen)
{
    if (plan->back) {
        PoolTakeBack(was, plan->back_len);
        FallowRetag(was->gen, gen);
    }
    if (PoolTailRoom() < r->bytes)
        (void)PoolTailTurn(plan->keep);
    return PoolAppend(r->packed, r->bytes, gen);
}

/* The file's row follows its record: where the table will name it the next
 * time a cycle writes its block. */
static void InkRowFollows(uint64_t fid, const RecordWhere* where)
{
    uint32_t block = 0;
    if (PoolLinkBlock(where->link, &block))
        (void)FileMapRowSet(InkState()->files, fid, block, where->offset);
}

/* The same laying and the same door as every Ink: a moved record is its
 * file's newest, and lands past every record there is. */
void InkGleanLocked(uint64_t fid, const RecordWhere* from, const uint8_t* packed)
{
    uint64_t    gen = s_inked_gen + 1;
    RecordWhere to  = PoolAppend(packed, from->len, gen);
    (void)PoolMirrorMove(fid, from, &to);
    __atomic_store_n(&s_inked_gen, gen, __ATOMIC_RELEASE);
}

// ----------------------------------------------------------------------------
// A Spare, when an Ink had none
// ----------------------------------------------------------------------------

/* What the loops below do with an answer that is not final: INK_AGAIN goes
 * round again; ERR_WOULD_BLOCK does too, once a Spare is made. A Spare the
 * volume has no ground for while a Settle cycle would give some back is the
 * Ink's ERR_WOULD_BLOCK, nothing changed: its caller waits for the cycle its
 * own way (helm.h, settle_ops.h) — no cycle is run beneath an Ink. */
static bool InkGoesRound(error_t* rc, InkSpares spares)
{
    if (*rc == INK_AGAIN)
        return true;
    if (*rc != ERR_WOULD_BLOCK || spares != INK_MAKES_SPARES)
        return false;
    *rc = PoolSpareMake();
    return *rc == OK;
}

// ----------------------------------------------------------------------------
// A Plot's size and extents
// ----------------------------------------------------------------------------

/* The record the Plot commits — the file's fields as `cur` has them, the
 * Plot's size and runs, and the time it was written — in both copies: the
 * medium's packed from the Plot's runs, as format v1 always packed them, and
 * memory's `cur` with what the Plot did to its runs spliced in, every Parcel
 * the Plot did not touch shared with `cur` (PlotConveyance). The ground the
 * splices free is not asked of them: Fallow is fed the Plot's own, as format
 * v1 always fed it (TagFSInkPlot). A STRICT or VOLUME_TESTS kernel holds the
 * two copies to each other, and the ground the splices free to the Plot's. */
static error_t InkPlotRecord(const Plot* p, const FileRecord* cur, uint64_t now, InkRecord* r)
{
    TagFSMetadata meta = {
        .file_id       = p->fid,
        .flags         = cur->flags,
        .size          = p->size,
        .created_time  = cur->created_time,
        .modified_time = now,
        .filename      = (char*)cur->name,
        .tag_ids       = (uint32_t*)cur->tags,
        .tag_count     = cur->tag_count,
        .extents       = p->runs,
        .extent_count  = p->run_count,
    };
    BookSplice* sp  = NULL;
    uint64_t    nsp = 0;
    error_t     rc  = InkRecordMake(&meta, r);
    if (rc == OK)
        rc = PlotConveyance(p, cur, &sp, &nsp);
    if (rc == OK)
        rc = FileRecordSplice(cur, sp, nsp, p->size, now, &r->rec, NULL, NULL);
#if CONFIG_STRICT || defined(CONFIG_VOLUME_TESTS)
    if (rc == OK)
        (void)PlotConveyanceAgrees(p, cur, sp, nsp, r->rec, r->packed, r->bytes);
#endif
    kfree(sp);
    if (rc != OK)
        InkRecordLetGo(r);
    return rc;
}

/* The second half, under the lock. */
static error_t InkPlotLocked(Plot* p, const RecordWhere* was, InkRecord* r, FallowNode* fallow,
                             InkReceipt* out)
{
    error_t rc = InkVolumeTakes();
    if (rc != OK)
        return rc;
    RecordWhere now;
    FileRecord* still = FileMapRecordHold(InkState()->files, p->fid, &now);
    if (!still)
        return ERR_FILE_NOT_FOUND;
    FileRecordRelease(still);
    if (now.gen != was->gen)
        return INK_AGAIN;

    InkPlan plan;
    rc = InkPlanFor(p->fid, &now, r->bytes, fallow ? INK_FREES_GROUND : INK_FREES_NOTHING,
                    &plan);
    if (rc != OK)
        return rc;

    uint64_t    gen   = s_inked_gen + 1;
    RecordWhere where = InkLayDown(&now, &plan, r, gen);
    where.shape = gen;
    FileMapStake none = { .count = 0 };
    (void)PoolMirrorPut(p->fid, r->rec, &where, 0, &none);
    r->rec = NULL;
    InkRowFollows(p->fid, &where);
    if (fallow)
        FallowAdd(fallow, gen);
    if (p->stake_count)
        BlockMapInkedLocked(p->fid, gen);    /* its Stakes reach the medium with this record */
    if (p->claimed)
        p->helm->claimed_runs = 0;    /* the record names them now */
    if (p->data_need > s_data_need)
        s_data_need = p->data_need;
    __atomic_store_n(&s_inked_gen, gen, __ATOMIC_RELEASE);
    out->gen = gen;
    return OK;
}

/* Whether the file wears settle:lazy — its program's own word that its changes
 * wait for the window. */
static bool InkWearsLazy(const FileRecord* cur)
{
    TagSpelling lazy = { .key = "settle", .key_len = 6, .value = "lazy", .value_len = 4 };
    uint32_t    id   = TagRegistryLookup(InkState()->registry, &lazy);
    if (id == TAGFS_INVALID_TAG_ID)
        return false;
    for (uint32_t i = 0; i < cur->tag_count; i++)
        if (cur->tags[i] == id)
            return true;
    return false;
}

/*
 * Whether a write is a frequent small writer's (settle.h), told by what it
 * does: it grew the file by data that cost the medium less time than the last
 * Settle cycle did — settling it would cost more than it — and the file's last
 * change was such an append too, less than a Lull ago. Both measures are the
 * medium's own, measured; nothing here is a number of bytes or of writes.
 * `small` says whether this one is such an append, for the next to ask.
 */
static bool InkPlotLazy(const Plot* p, const FileRecord* cur, uint64_t now_us, bool* small)
{
    uint64_t d = SettleLastDUs();
    *small = p->size > p->size_before && d != 0 && p->data_us < d;
    if (InkWearsLazy(cur))
        return true;
    return *small && cur->small_append_us != 0 && now_us > cur->small_append_us &&
           now_us - cur->small_append_us < SettleLullUs();
}

/* The tags the Plot was loaded with are the ones asked, with the warrant it
 * was loaded with: a file that came to wear a guarded tag after the load had
 * its bytes laid already, and refusing the record now would take back its
 * size, not its bytes. */
error_t TagFSInkPlot(Plot* p, InkSpares spares, InkReceipt* out)
{
    error_t rc = InkGuard(p->fid, p->meta.tag_ids, p->meta.tag_count, p->runs, p->run_count,
                          p->freed, p->freed_count, p->warrant);
    if (rc != OK)
        return rc;

    FallowNode* fallow = NULL;
    if (p->freed_count &&
        !(fallow = FallowNodeMake(p->freed, p->freed_count, FALLOW_BY_RECORD, p->fid)))
        return ERR_NO_MEMORY;

    uint64_t now    = rtc_get_unix64();
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
        InkRecord r;
        rc = InkPlotRecord(p, cur, now, &r);
        if (rc == OK) {
            r.rec->small_append_us = small ? now_us : 0;
            TagFSLockTake(&InkState()->lock);
            rc = InkPlotLocked(p, &was, &r, fallow, out);
            TagFSLockDrop(&InkState()->lock);
        }
        /* The record this Ink replaced goes here, after the lock: `cur` holds
         * it — the map's record while `was` is the map's place for it — so
         * the map lets go of it under the lock without freeing a byte, and
         * what only it named is freed now, with no lock held. */
        FileRecordRelease(cur);
        InkRecordLetGo(&r);
        if (rc == OK) {
            fallow = NULL;
            break;
        }
        if (!InkGoesRound(&rc, spares))
            break;
    }
    FallowNodeLetGo(fallow);
    if (rc != OK)
        return rc;

    p->claimed       = 0;
    p->stake_count   = 0;
    p->freed_count   = 0;
    p->size_before   = p->size;
    p->blocks_before = p->blocks;
    p->runs_before   = p->run_count;
    p->shape         = out->gen;
    p->data_need     = 0;
    p->data_us       = 0;
    SettleInked(out->gen, lazy);
    return OK;
}

/* Under the state lock, so a name or a tag inked for the file either came
 * before — and the record as it is now is the one checked here — or comes
 * after, and finds the room claimed. */
error_t InkRoomClaim(Plot* p)
{
    uint64_t extents = PackV1Extents(p->runs, p->run_count);
    if (!p->helm || extents <= p->runs_before)
        return OK;

    TagFSState* fs = InkState();
    error_t     rc = OK;
    TagFSLockTake(&fs->lock);
    FileRecord* cur = FileMapRecordHold(fs->files, p->fid, NULL);
    if (!cur) {
        rc = ERR_FILE_NOT_FOUND;
    } else {
        TagFSMetadata record = {
            .filename     = (char*)cur->name,
            .tag_ids      = (uint32_t*)cur->tags,
            .tag_count    = cur->tag_count,
            .extents      = p->runs,
            .extent_count = p->run_count,
        };
        if (PlotFits(&record, NULL)) {
            /* A record that fits carries at most a u16 of extents. */
            p->helm->claimed_runs = (uint32_t)extents;
            p->claimed            = (uint32_t)extents;
        } else {
            rc = ERR_TAGFS_RECORD_FULL;
        }
        FileRecordRelease(cur);
    }
    TagFSLockDrop(&fs->lock);
    if (rc == ERR_TAGFS_RECORD_FULL)
        kprintf("[TagFS] file %llu: its name and tags as they are now leave its record no room "
                "for the %llu extent(s) this call would name — refused, and nothing of it "
                "written\n", p->fid, (unsigned long long)extents);
    return rc;
}

void InkRoomLetGo(Plot* p)
{
    if (!p->claimed)
        return;
    TagFSLockTake(&InkState()->lock);
    p->helm->claimed_runs = 0;
    TagFSLockDrop(&InkState()->lock);
    p->claimed = 0;
}

// ----------------------------------------------------------------------------
// A file's name and tags
// ----------------------------------------------------------------------------

typedef struct {
    uint32_t* tags;
    uint32_t  count;
    uint32_t  dropped[2];   /* the tags that come off the index */
    uint32_t  drops;
    bool      adds;         /* `add` goes onto the index */
} InkFieldsPlan;

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

/* The record `meta` describes must also leave the room a write in flight
 * claimed (RT-19): the extents that write will ink, where they are more than
 * the record names now. */
static bool InkFieldsLeaveRoom(uint64_t fid, const TagFSMetadata* meta)
{
    uint32_t claimed = HelmClaimedRuns(fid);
    return claimed <= PackV1Extents(meta->extents, meta->extent_count) ||
           claimed <= PlotRunRoom(meta);
}

static error_t InkFieldsLocked(uint64_t fid, const RecordWhere* was, const InkFieldsPlan* f,
                               const TagFSMetadata* meta, uint32_t add, InkRecord* r,
                               TagBitmapStake* stake, InkReceipt* out)
{
    TagFSState* fs = InkState();
    error_t     rc = InkVolumeTakes();
    if (rc != OK)
        return rc;
    RecordWhere now;
    FileRecord* still = FileMapRecordHold(fs->files, fid, &now);
    if (!still)
        return ERR_FILE_NOT_FOUND;
    FileRecordRelease(still);
    if (now.gen != was->gen)
        return INK_AGAIN;
    if (f->adds && !TagBitmapStakeCovers(fs->bitmap_index, &add, 1, fid, stake))
        return INK_AGAIN;
    if (!InkFieldsLeaveRoom(fid, meta))
        return ERR_TAGFS_RECORD_FULL;

    InkPlan plan;
    rc = InkPlanFor(fid, &now, r->bytes, INK_FREES_NOTHING, &plan);
    if (rc != OK)
        return rc;

    uint64_t    gen   = s_inked_gen + 1;
    RecordWhere where = InkLayDown(&now, &plan, r, gen);
    where.shape = now.shape;
    FileMapStake none = { .count = 0 };
    (void)PoolMirrorPut(fid, r->rec, &where, 0, &none);
    r->rec = NULL;
    InkRowFollows(fid, &where);
    if (f->adds)
        TagBitmapAddCovered(fs->bitmap_index, &add, 1, fid);
    for (uint32_t i = 0; i < f->drops && i < 2; i++)
        TagBitmapLeave(fs->bitmap_index, f->dropped[i], fid);
    __atomic_store_n(&s_inked_gen, gen, __ATOMIC_RELEASE);
    out->gen = gen;
    return OK;
}

/* One round of InkFields: OK, a final refusal, or INK_AGAIN / ERR_WOULD_BLOCK. */
static error_t InkFieldsOnce(uint64_t fid, const char* name, uint32_t add, uint32_t drop,
                             InkWarrant warrant, InkReceipt* out)
{
    TagFSState* fs = InkState();
    RecordWhere was;
    FileRecord* cur = FileMapRecordHold(fs->files, fid, &was);
    if (!cur)
        return ERR_FILE_NOT_FOUND;

    InkFieldsPlan   f;
    InkRecord       r     = { 0 };
    TagBitmapStake* stake = NULL;
    Strip*          runs  = NULL;
    uint32_t        nruns = 0;
    error_t         rc    = InkVolumeBegins();
    if (!InkFieldsTags(cur, name, add, drop, &f)) {
        FileRecordRelease(cur);
        return ERR_NO_MEMORY;
    }
    TagFSMetadata meta = {
        .file_id       = fid,
        .flags         = cur->flags,
        .size          = cur->size,
        .created_time  = cur->created_time,
        .modified_time = cur->modified_time,
        .filename      = (char*)(name ? name : cur->name),
        .tag_ids       = f.tags,
        .tag_count     = f.count,
    };
    for (uint32_t i = 0; i < f.drops && i < 2 && rc == OK; i++)
        if (warrant != INK_WARRANTED && InkTagGuarded(f.dropped[i]))
            rc = ERR_PERMISSION_DENIED;
    if (rc == OK && f.adds && warrant != INK_WARRANTED && InkTagGuarded(add))
        rc = ERR_PERMISSION_DENIED;
    if (rc == OK && !name && !f.adds && f.drops == 0) {
        rc      = drop != TAGFS_INVALID_TAG_ID ? ERR_TAGFS_TAG_NOT_FOUND : OK;
        out->gen = was.gen;
        goto out;
    }
    /* The room format v1 judges first, on the extents the record names, as it
     * always did before it allocated more than the tags' list: counted where
     * the record holds its runs. Only a record that fits is laid flat for the
     * packer; memory's new one shares every run with `cur`. */
    if (rc == OK && !PlotFitsExtents(&meta, PackV1RecordExtents(cur), NULL))
        rc = ERR_TAGFS_RECORD_FULL;
    if (rc == OK && (rc = FileRecordFlatten(cur, &runs, &nruns)) == OK) {
        meta.extents      = runs;
        meta.extent_count = nruns;
        rc = InkRecordMake(&meta, &r);
    }
    if (rc == OK && !(r.rec = FileRecordRelabel(cur, meta.filename, f.tags, f.count, cur->flags)))
        rc = ERR_NO_MEMORY;
#if CONFIG_STRICT || defined(CONFIG_VOLUME_TESTS)
    if (rc == OK)
        (void)PackV1Agrees(r.rec, r.packed, r.bytes);
#endif
    if (rc == OK && f.adds)
        rc = TagBitmapStakeFor(fs->bitmap_index, &add, 1, fid, &stake);
    if (rc == OK) {
        TagFSLockTake(&fs->lock);
        rc = InkFieldsLocked(fid, &was, &f, &meta, add, &r, stake, out);
        TagFSLockDrop(&fs->lock);
        if (rc == OK) {
            SettleInked(out->gen, false);
            TagBitmapShrink(fs->bitmap_index, &fs->lock, f.dropped, f.drops < 2 ? f.drops : 2, fid);
        }
    }
out:
    if (rc == ERR_PERMISSION_DENIED)
        kprintf("[TagFS] file %llu: a tag of key system, boot or god goes on it or comes off it "
                "only with the warrant, and this call carries none\n", fid);
    else if (rc == ERR_TAGFS_RECORD_FULL)
        kprintf("[TagFS] file %llu: its record has no room for this name or tag beside the extents "
                "it names and those a write in flight will name — refused\n", fid);
    TagBitmapStakeLetGo(stake);
    InkRecordLetGo(&r);
    kfree(f.tags);
    kfree(runs);
    FileRecordRelease(cur);
    return rc;
}

error_t InkFields(uint64_t fid, const char* name, uint32_t add, uint32_t drop, InkWarrant warrant,
                  InkReceipt* out)
{
    error_t rc;
    do {
        rc = InkFieldsOnce(fid, name, add, drop, warrant, out);
    } while (rc != OK && InkGoesRound(&rc, INK_MAKES_SPARES));
    return rc;
}

// ----------------------------------------------------------------------------
// A new file
// ----------------------------------------------------------------------------

typedef struct {
    FileMapStake    map;
    TagBitmapStake* index;
} InkNewStakes;

static error_t InkNewLocked(const uint32_t* tags, uint32_t count, InkRecord* r,
                            InkNewStakes* st, uint64_t* out_fid, InkReceipt* out)
{
    TagFSState* fs  = InkState();
    uint64_t    fid = fs->ledger.next_file_id;
    error_t     rc  = InkVolumeTakes();
    if (rc != OK)
        return rc;

    if (FileMapNodesToAdd(fs->files, fid) > st->map.count ||
        !TagBitmapStakeCovers(fs->bitmap_index, tags, count, fid, st->index))
        return INK_AGAIN;
    /* The id is issued as the last of the checks: a refused one finds nothing
     * changed, and an issued one is the id the stakes were taken for. */
    InkPlan plan;
    rc = InkPlanFor(fid, NULL, r->bytes, INK_FREES_NOTHING, &plan);
    if (rc == OK)
        rc = PackV1Refile(r->packed, fid);    /* the id the issue gives, as the record carries it */
    if (rc == OK)
        rc = LedgerIssueFileId(&fs->ledger, &fid);
    if (rc != OK)
        return rc;

    r->rec->file_id = fid;

    uint64_t    gen   = s_inked_gen + 1;
    RecordWhere where = InkLayDown(NULL, &plan, r, gen);
    uint32_t    block = 0;
    where.shape = gen;
    (void)PoolLinkBlock(where.link, &block);
    (void)PoolMirrorPut(fid, r->rec, &where, block, &st->map);   /* the row with the record */
    r->rec = NULL;
    (void)file_table_update(fid, block, where.offset);
    TagBitmapAddCovered(fs->bitmap_index, tags, count, fid);
    fs->ledger.total_files++;
    __atomic_store_n(&s_inked_gen, gen, __ATOMIC_RELEASE);
    *out_fid = fid;
    out->gen = gen;
    return OK;
}

error_t InkNew(const char* name, const uint32_t* tags, uint32_t count, uint64_t* out_fid,
               InkReceipt* out)
{
    TagFSState* fs  = InkState();
    uint64_t    now = rtc_get_unix64();
    TagFSMetadata meta = {
        .file_id       = 0,          /* the id is the lock's to give: patched in there */
        .flags         = TAGFS_FILE_ACTIVE,
        .created_time  = now,
        .modified_time = now,
        .filename      = (char*)name,
        .tag_ids       = (uint32_t*)tags,
        .tag_count     = count,
    };

    error_t rc = InkVolumeBegins();
    if (rc != OK)
        return rc;
    InkRecord r;
    rc = InkRecordMake(&meta, &r);
    if (rc == OK && !(r.rec = FileRecordBuild(&meta))) {
        InkRecordLetGo(&r);
        rc = ERR_NO_MEMORY;
    }
#if CONFIG_STRICT || defined(CONFIG_VOLUME_TESTS)
    if (rc == OK)
        (void)PackV1Agrees(r.rec, r.packed, r.bytes);
#endif
    if (rc == ERR_TAGFS_RECORD_FULL)
        kprintf("[TagFS] create of '%s': %u tag(s) and its name make a record wider than this "
                "volume holds (%u bytes) — no file is made\n", name, count, TAGFS_MPOOL_DATA_SIZE);
    if (rc != OK)
        return rc;

    for (;;) {
        InkNewStakes st = { .map = { .count = 0 }, .index = NULL };
        TagFSLockTake(&fs->lock);
        uint64_t fid = fs->ledger.next_file_id;
        TagFSLockDrop(&fs->lock);

        rc = FileMapStakeFor(fs->files, fid, &st.map);
        if (rc == OK)
            rc = TagBitmapStakeFor(fs->bitmap_index, tags, count, fid, &st.index);
        if (rc == OK) {
            TagFSLockTake(&fs->lock);
            rc = InkNewLocked(tags, count, &r, &st, out_fid, out);
            TagFSLockDrop(&fs->lock);
            if (rc == OK)
                SettleInked(out->gen, false);
        }
        FileMapStakeLetGo(&st.map);
        TagBitmapStakeLetGo(st.index);
        if (rc == OK || !InkGoesRound(&rc, INK_MAKES_SPARES))
            break;
    }
    InkRecordLetGo(&r);
    return rc;
}

// ----------------------------------------------------------------------------
// A file given up
// ----------------------------------------------------------------------------

static error_t InkQuitclaimLocked(uint64_t fid, const RecordWhere* was, const FileRecord* cur,
                                  InkRecord* r, FallowNode* fallow, InkReceipt* out)
{
    TagFSState* fs = InkState();
    error_t     rc = InkVolumeTakes();
    if (rc != OK)
        return rc;
    RecordWhere now;
    FileRecord* still = FileMapRecordHold(fs->files, fid, &now);
    if (!still)
        return ERR_FILE_NOT_FOUND;
    FileRecordRelease(still);
    if (now.gen != was->gen)
        return INK_AGAIN;

    InkPlan plan;
    rc = InkPlanFor(fid, &now, r->bytes, fallow ? INK_FREES_GROUND : INK_GIVES_UP_NOTHING,
                    &plan);
    if (rc != OK)
        return rc;

    uint64_t    gen   = s_inked_gen + 1;
    RecordWhere where = InkLayDown(&now, &plan, r, gen);
    FileMapStake none = { .count = 0 };
    (void)PoolMirrorPut(fid, NULL, &where, 0, &none);
    for (uint32_t i = 0; i < cur->tag_count; i++)
        TagBitmapLeave(fs->bitmap_index, cur->tags[i], fid);
    if (fallow)
        FallowAdd(fallow, gen);
    AllotmentRelease(fid);
    if (fs->ledger.total_files > 0)
        fs->ledger.total_files--;
    __atomic_store_n(&s_inked_gen, gen, __ATOMIC_RELEASE);
    out->gen = gen;
    return OK;
}

/* One round of InkQuitclaim. */
static error_t InkQuitclaimOnce(uint64_t fid, InkWarrant warrant, InkReceipt* out)
{
    TagFSState* fs = InkState();
    RecordWhere was;
    FileRecord* cur = FileMapRecordHold(fs->files, fid, &was);
    if (!cur)
        return ERR_FILE_NOT_FOUND;

    /* Guarded on the record where it holds its runs, with no allocation:
     * format v1 refused a file the system keeps, or the kernel's, before it
     * asked the heap for anything, and a heap with nothing left turns neither
     * refusal into another. The ground let go is walked the same way. */
    FallowNode* fallow = NULL;
    InkRecord   r      = { 0 };
    error_t     rc     = InkVolumeBegins();
    if (rc == OK)
        rc = InkGuardSays(fid, InkRecordTouchesBoot(cur), cur->tags, cur->tag_count, warrant);
    if (rc == OK)
        rc = FallowNodeOfRecord(cur, FALLOW_BY_RECORD, &fallow);
    if (rc == OK) {
        TagFSMetadata quitclaim = {
            .file_id       = fid,
            .flags         = 0,
            .created_time  = cur->created_time,
            .modified_time = rtc_get_unix64(),
        };
        rc = InkRecordMake(&quitclaim, &r);
    }
    if (rc == OK) {
        TagFSLockTake(&fs->lock);
        rc = InkQuitclaimLocked(fid, &was, cur, &r, fallow, out);
        TagFSLockDrop(&fs->lock);
        if (rc == OK) {
            fallow = NULL;
            SettleInked(out->gen, false);
            TagBitmapShrink(fs->bitmap_index, &fs->lock, cur->tags, cur->tag_count, fid);
        }
    }
    FallowNodeLetGo(fallow);
    InkRecordLetGo(&r);
    FileRecordRelease(cur);
    return rc;
}

/* A holder of the file's Helm is not waited for: the file is vacated, and the
 * holder answers ERR_FILE_NOT_FOUND at its own Ink, its Stakes given back. A
 * volume with no block to make a Spare from is no answer while Glean can make
 * the room: the delete waits for its sweep (glean.h). */
error_t InkQuitclaim(uint64_t fid, InkWarrant warrant, InkReceipt* out)
{
    error_t rc;
    do {
        rc = InkQuitclaimOnce(fid, warrant, out);
    } while (rc != OK && InkGoesRound(&rc, INK_MAKES_SPARES));
    if (rc == ERR_TAGFS_NO_SPACE && GleanSweepAsk())
        rc = ERR_WOULD_BLOCK;
    if (rc == OK)
        HelmVacate(fid);
    return rc;
}
