#include "ink.h"
#include "fallow.h"
#include "../metadata_pool/meta_pool.h"
#include "../file_map/file_map.h"
#include "../file_table/file_table.h"
#include "../tag_registry/tag_registry.h"
#include "../settle/settle.h"
#include "../helm/helm.h"
#include "../../../kernel/drivers/timer/rtc.h"

/*
 * The halves of every Ink (ink.h): the record built with no lock held, then
 * laid, named and indexed under the volume's state lock with nothing left that
 * can refuse. The counters below move under that lock alone.
 */

static uint64_t s_inked_gen;     /* the newest Ink's */
static uint64_t s_data_need;     /* the flush need the newest data an Ink named holds */

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
    s_data_need = 0;
}

static TagFSState* InkState(void)
{
    return tagfs_get_state();
}

// ----------------------------------------------------------------------------
// The guards (RT-09)
// ----------------------------------------------------------------------------

bool InkTagGuarded(uint16_t tag_id)
{
    TagSpelling tag = { 0 };
    if (tag_id == TAGFS_INVALID_TAG_ID ||
        !TagRegistrySpelling(InkState()->registry, tag_id, &tag, NULL))
        return false;
    return TagSpanIs("system", tag.key, tag.key_len) || TagSpanIs("boot", tag.key, tag.key_len) ||
           TagSpanIs("god", tag.key, tag.key_len);
}

bool InkTouchesBoot(const FileExtent* runs, uint32_t count)
{
    TagFSState* fs = InkState();
    if (!fs->boot.kernel_blocks || fs->boot.kernel_block < fs->layout.data_block)
        return false;
    uint64_t first = fs->boot.kernel_block - fs->layout.data_block;
    uint64_t end   = first + fs->boot.kernel_blocks;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t a = runs[i].start_block, b = a + runs[i].block_count;
        if (a < end && b > first)
            return true;
    }
    return false;
}

static bool InkWearsGuarded(const FileRecord* rec)
{
    for (uint32_t i = 0; i < rec->tag_count; i++)
        if (InkTagGuarded(rec->tags[i]))
            return true;
    return false;
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

/* `meta` built into both copies of the record: OK, ERR_TAGFS_RECORD_FULL for
 * a record wider than format v1 holds, ERR_NO_MEMORY. */
static error_t InkRecordMake(const TagFSMetadata* meta, bool keep, InkRecord* r)
{
    memset(r, 0, sizeof(*r));
    if (!PlotFits(meta, &r->bytes))
        return ERR_TAGFS_RECORD_FULL;
    r->packed = kmalloc(r->bytes);
    if (!r->packed)
        return ERR_NO_MEMORY;
    if (PackV1(meta, r->packed) != r->bytes) {
        InkRecordLetGo(r);
        return ERR_TAGFS_METADATA_ERROR;
    }
    if (keep && !(r->rec = FileRecordOf(meta))) {
        InkRecordLetGo(r);
        return ERR_NO_MEMORY;
    }
    return OK;
}

// ----------------------------------------------------------------------------
// The tail, under the lock
// ----------------------------------------------------------------------------

typedef struct {
    bool     back;          /* the file's record in the tail is taken back first */
    uint32_t back_len;
} InkPlan;

/* Room for `bytes`, planned and nothing changed: ERR_WOULD_BLOCK when the tail
 * has too little and no Spare may be had. */
static error_t InkPlanFor(uint32_t fid, const RecordWhere* was, uint32_t bytes, bool dig,
                          InkPlan* plan)
{
    plan->back     = was && PoolTailHolds(was, fid, &plan->back_len);
    uint32_t room  = PoolTailRoom() + (plan->back ? plan->back_len : 0);
    return room >= bytes || PoolSpareAvailable(dig) ? OK : ERR_WOULD_BLOCK;
}

/* The plan carried out and the record laid: where it sits. From here nothing
 * refuses. What the replaced record had freed is freed by this Ink now. */
static RecordWhere InkLayDown(const RecordWhere* was, const InkPlan* plan, const InkRecord* r,
                              bool dig, uint64_t gen)
{
    if (plan->back) {
        PoolTakeBack(was, plan->back_len);
        FallowRetag(was->gen, gen);
    }
    if (PoolTailRoom() < r->bytes)
        (void)PoolTailTurn(dig);
    return PoolAppend(r->packed, r->bytes, gen);
}

/* The file's row follows its record: where the table will name it the next
 * time a cycle writes its block. */
static void InkRowFollows(uint32_t fid, const RecordWhere* where)
{
    uint32_t block = 0;
    if (PoolLinkBlock(where->link, &block))
        (void)FileMapRowSet(InkState()->files, fid, block, where->offset);
}

// ----------------------------------------------------------------------------
// A Spare, when an Ink had none
// ----------------------------------------------------------------------------

/* One Spare made; and when the volume is short while Fallow rests blocks, a
 * caller that may wait has cycles run until Fallow gives them back, and tries
 * once more. */
static error_t InkSpareFor(void)
{
    error_t rc = PoolSpareMake();
    if (rc == ERR_WOULD_BLOCK) {
        rc = SettleForRoom();
        if (rc == OK)
            rc = PoolSpareMake();
    }
    return rc;
}

/* What the loops below do with an answer that is not final: INK_AGAIN goes
 * round again; ERR_WOULD_BLOCK does too, once a Spare is made. */
static bool InkGoesRound(error_t* rc, InkSpares spares)
{
    if (*rc == INK_AGAIN)
        return true;
    if (*rc != ERR_WOULD_BLOCK || spares != INK_MAKES_SPARES)
        return false;
    *rc = InkSpareFor();
    return *rc == OK;
}

// ----------------------------------------------------------------------------
// A Plot's size and extents
// ----------------------------------------------------------------------------

/* The record the Plot commits: the file's fields as `cur` has them, the Plot's
 * size and runs, and the time it was written. */
static error_t InkPlotRecord(const Plot* p, const FileRecord* cur, uint64_t now, InkRecord* r)
{
    TagFSMetadata meta = {
        .file_id       = p->fid,
        .flags         = cur->flags,
        .size          = p->size,
        .created_time  = cur->created_time,
        .modified_time = now,
        .filename      = (char*)cur->name,
        .tag_ids       = (uint16_t*)cur->tags,
        .tag_count     = cur->tag_count,
        .extents       = p->runs,
        .extent_count  = p->run_count,
    };
    return InkRecordMake(&meta, true, r);
}

/* The second half, under the lock. */
static error_t InkPlotLocked(Plot* p, const RecordWhere* was, InkRecord* r, FallowNode* fallow,
                             InkReceipt* out)
{
    RecordWhere now;
    FileRecord* still = FileMapRecordHold(InkState()->files, p->fid, &now);
    if (!still)
        return ERR_FILE_NOT_FOUND;
    FileRecordRelease(still);
    if (now.gen != was->gen)
        return INK_AGAIN;

    InkPlan plan;
    error_t rc = InkPlanFor(p->fid, &now, r->bytes, false, &plan);
    if (rc != OK)
        return rc;

    uint64_t    gen   = s_inked_gen + 1;
    RecordWhere where = InkLayDown(&now, &plan, r, false, gen);
    where.shape = gen;
    FileMapStake none = { .count = 0 };
    (void)PoolMirrorPut(p->fid, r->rec, &where, &none);
    r->rec = NULL;
    InkRowFollows(p->fid, &where);
    if (fallow)
        FallowAdd(fallow, gen);
    if (p->data_need > s_data_need)
        s_data_need = p->data_need;
    __atomic_store_n(&s_inked_gen, gen, __ATOMIC_RELEASE);
    out->gen = gen;
    return OK;
}

error_t TagFSInkPlot(Plot* p, InkSpares spares, InkReceipt* out)
{
    if (InkTouchesBoot(p->runs, p->run_count) || InkTouchesBoot(p->freed, p->freed_count)) {
        kprintf("[TagFS] file %u holds the kernel the loader reads by its blocks — its "
                "extents are not changed\n", p->fid);
        return ERR_PERMISSION_DENIED;
    }

    FallowNode* fallow = NULL;
    if (p->freed_count &&
        !(fallow = FallowNodeMake(p->freed, p->freed_count, FALLOW_BY_RECORD, p->fid)))
        return ERR_NO_MEMORY;

    uint64_t now = rtc_get_unix64();
    error_t  rc;
    for (;;) {
        RecordWhere was;
        FileRecord* cur = FileMapRecordHold(InkState()->files, p->fid, &was);
        if (!cur) {
            rc = ERR_FILE_NOT_FOUND;
            break;
        }
        if (was.shape != p->shape) {
            FileRecordRelease(cur);
            kprintf("[TagFS] file %u: its extents were changed since this call loaded them, by a "
                    "caller that did not hold its Helm — this record is refused rather than laid "
                    "over that one\n", p->fid);
            rc = ERR_BUSY;
            break;
        }
        InkRecord r;
        rc = InkPlotRecord(p, cur, now, &r);
        FileRecordRelease(cur);
        if (rc != OK)
            break;

        TagFSLockTake(&InkState()->lock);
        rc = InkPlotLocked(p, &was, &r, fallow, out);
        TagFSLockDrop(&InkState()->lock);
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

    p->stake_count   = 0;
    p->freed_count   = 0;
    p->size_before   = p->size;
    p->blocks_before = p->blocks;
    p->runs_before   = p->run_count;
    p->shape         = out->gen;
    p->data_need     = 0;
    return OK;
}

// ----------------------------------------------------------------------------
// A file's name and tags
// ----------------------------------------------------------------------------

typedef struct {
    uint16_t* tags;
    uint32_t  count;
    uint16_t  dropped[2];   /* the tags that come off the index */
    uint32_t  drops;
    bool      adds;         /* `add` goes onto the index */
} InkFieldsPlan;

/* The tags the file wears after the change, into a list of its own; false
 * for no memory. */
static bool InkFieldsTags(const FileRecord* cur, const char* name, uint16_t add, uint16_t drop,
                          InkFieldsPlan* f)
{
    uint16_t old_name = name ? InkNameTagLookup(cur->name) : TAGFS_INVALID_TAG_ID;
    memset(f, 0, sizeof(*f));
    f->tags = kmalloc(sizeof(uint16_t) * (cur->tag_count + 1u));
    if (!f->tags)
        return false;

    bool worn = false;
    if (name && add != TAGFS_INVALID_TAG_ID)
        f->tags[f->count++] = add;
    for (uint32_t i = 0; i < cur->tag_count; i++) {
        uint16_t t = cur->tags[i];
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

static error_t InkFieldsLocked(uint32_t fid, const RecordWhere* was, const InkFieldsPlan* f,
                               uint16_t add, InkRecord* r, TagBitmapStake* stake,
                               InkReceipt* out)
{
    TagFSState* fs = InkState();
    RecordWhere now;
    FileRecord* still = FileMapRecordHold(fs->files, fid, &now);
    if (!still)
        return ERR_FILE_NOT_FOUND;
    FileRecordRelease(still);
    if (now.gen != was->gen)
        return INK_AGAIN;
    if (f->adds && !TagBitmapStakeCovers(fs->bitmap_index, &add, 1, fid, stake))
        return INK_AGAIN;

    InkPlan plan;
    error_t rc = InkPlanFor(fid, &now, r->bytes, false, &plan);
    if (rc != OK)
        return rc;

    uint64_t    gen   = s_inked_gen + 1;
    RecordWhere where = InkLayDown(&now, &plan, r, false, gen);
    where.shape = now.shape;
    FileMapStake none = { .count = 0 };
    (void)PoolMirrorPut(fid, r->rec, &where, &none);
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
static error_t InkFieldsOnce(uint32_t fid, const char* name, uint16_t add, uint16_t drop,
                             InkReceipt* out)
{
    TagFSState* fs = InkState();
    RecordWhere was;
    FileRecord* cur = FileMapRecordHold(fs->files, fid, &was);
    if (!cur)
        return ERR_FILE_NOT_FOUND;

    InkFieldsPlan   f;
    InkRecord       r     = { 0 };
    TagBitmapStake* stake = NULL;
    error_t         rc    = OK;
    if (!InkFieldsTags(cur, name, add, drop, &f)) {
        FileRecordRelease(cur);
        return ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < f.drops && i < 2 && rc == OK; i++)
        if (InkTagGuarded(f.dropped[i]))
            rc = ERR_PERMISSION_DENIED;
    if (rc == OK && !name && !f.adds && f.drops == 0) {
        rc      = drop != TAGFS_INVALID_TAG_ID ? ERR_TAGFS_TAG_NOT_FOUND : OK;
        out->gen = was.gen;
        goto out;
    }
    if (rc == OK) {
        TagFSMetadata meta = {
            .file_id       = fid,
            .flags         = cur->flags,
            .size          = cur->size,
            .created_time  = cur->created_time,
            .modified_time = cur->modified_time,
            .filename      = (char*)(name ? name : cur->name),
            .tag_ids       = f.tags,
            .tag_count     = f.count,
            .extents       = (FileExtent*)cur->extents,
            .extent_count  = cur->extent_count,
        };
        rc = InkRecordMake(&meta, true, &r);
    }
    if (rc == OK && f.adds)
        rc = TagBitmapStakeFor(fs->bitmap_index, &add, 1, fid, &stake);
    if (rc == OK) {
        TagFSLockTake(&fs->lock);
        rc = InkFieldsLocked(fid, &was, &f, add, &r, stake, out);
        TagFSLockDrop(&fs->lock);
    }
out:
    if (rc == ERR_PERMISSION_DENIED)
        kprintf("[TagFS] file %u: a tag of key system, boot or god does not come off it\n", fid);
    TagBitmapStakeLetGo(stake);
    InkRecordLetGo(&r);
    kfree(f.tags);
    FileRecordRelease(cur);
    return rc;
}

error_t InkFields(uint32_t fid, const char* name, uint16_t add, uint16_t drop, InkReceipt* out)
{
    error_t rc;
    do {
        rc = InkFieldsOnce(fid, name, add, drop, out);
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

static error_t InkNewLocked(const uint16_t* tags, uint32_t count, InkRecord* r,
                            InkNewStakes* st, uint32_t* out_fid, InkReceipt* out)
{
    TagFSState* fs  = InkState();
    uint32_t    fid = fs->ledger.next_file_id;

    if (FileMapNodesToAdd(fs->files, fid) > st->map.count ||
        !TagBitmapStakeCovers(fs->bitmap_index, tags, count, fid, st->index))
        return INK_AGAIN;
    /* The id is issued as the last of the checks: a refused one finds nothing
     * changed, and an issued one is the id the stakes were taken for. */
    InkPlan plan;
    error_t rc = InkPlanFor(fid, NULL, r->bytes, false, &plan);
    if (rc == OK)
        rc = LedgerIssueFileId(&fs->ledger, &fid);
    if (rc != OK)
        return rc;

    PackV1Refile(r->packed, fid);
    r->rec->file_id = fid;

    uint64_t    gen   = s_inked_gen + 1;
    RecordWhere where = InkLayDown(NULL, &plan, r, false, gen);
    where.shape = gen;
    (void)PoolMirrorPut(fid, r->rec, &where, &st->map);
    r->rec = NULL;
    uint32_t block = 0;
    (void)PoolLinkBlock(where.link, &block);
    (void)file_table_update(fid, block, where.offset);
    TagBitmapAddCovered(fs->bitmap_index, tags, count, fid);
    fs->ledger.total_files++;
    __atomic_store_n(&s_inked_gen, gen, __ATOMIC_RELEASE);
    *out_fid = fid;
    out->gen = gen;
    return OK;
}

error_t InkNew(const char* name, const uint16_t* tags, uint32_t count, uint32_t* out_fid,
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
        .tag_ids       = (uint16_t*)tags,
        .tag_count     = count,
    };

    InkRecord r;
    error_t   rc = InkRecordMake(&meta, true, &r);
    if (rc == ERR_TAGFS_RECORD_FULL)
        kprintf("[TagFS] create of '%s': %u tag(s) and its name make a record wider than this "
                "volume holds (%u bytes) — no file is made\n", name, count, TAGFS_MPOOL_DATA_SIZE);
    if (rc != OK)
        return rc;

    for (;;) {
        InkNewStakes st = { .map = { .count = 0 }, .index = NULL };
        TagFSLockTake(&fs->lock);
        uint32_t fid = fs->ledger.next_file_id;
        TagFSLockDrop(&fs->lock);

        rc = FileMapStakeFor(fs->files, fid, &st.map);
        if (rc == OK)
            rc = TagBitmapStakeFor(fs->bitmap_index, tags, count, fid, &st.index);
        if (rc == OK) {
            TagFSLockTake(&fs->lock);
            rc = InkNewLocked(tags, count, &r, &st, out_fid, out);
            TagFSLockDrop(&fs->lock);
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

static error_t InkQuitclaimLocked(uint32_t fid, const RecordWhere* was, const FileRecord* cur,
                                  InkRecord* r, FallowNode* fallow, InkReceipt* out)
{
    TagFSState* fs = InkState();
    RecordWhere now;
    FileRecord* still = FileMapRecordHold(fs->files, fid, &now);
    if (!still)
        return ERR_FILE_NOT_FOUND;
    FileRecordRelease(still);
    if (now.gen != was->gen)
        return INK_AGAIN;

    InkPlan plan;
    error_t rc = InkPlanFor(fid, &now, r->bytes, true, &plan);
    if (rc != OK)
        return rc;

    uint64_t    gen   = s_inked_gen + 1;
    RecordWhere where = InkLayDown(&now, &plan, r, true, gen);
    FileMapStake none = { .count = 0 };
    (void)PoolMirrorPut(fid, NULL, &where, &none);
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
static error_t InkQuitclaimOnce(uint32_t fid, InkReceipt* out)
{
    TagFSState* fs = InkState();
    RecordWhere was;
    FileRecord* cur = FileMapRecordHold(fs->files, fid, &was);
    if (!cur)
        return ERR_FILE_NOT_FOUND;

    FallowNode* fallow = NULL;
    InkRecord   r      = { 0 };
    error_t     rc     = OK;
    if (InkWearsGuarded(cur) || InkTouchesBoot(cur->extents, cur->extent_count)) {
        kprintf("[TagFS] file %u wears system, boot or god, or holds the kernel the loader "
                "reads — it is not given up\n", fid);
        rc = ERR_PERMISSION_DENIED;
    }
    if (rc == OK && cur->extent_count &&
        !(fallow = FallowNodeMake(cur->extents, cur->extent_count, FALLOW_BY_RECORD, fid)))
        rc = ERR_NO_MEMORY;
    if (rc == OK) {
        TagFSMetadata quitclaim = {
            .file_id       = fid,
            .flags         = 0,
            .created_time  = cur->created_time,
            .modified_time = rtc_get_unix64(),
        };
        rc = InkRecordMake(&quitclaim, false, &r);
    }
    if (rc == OK) {
        TagFSLockTake(&fs->lock);
        rc = InkQuitclaimLocked(fid, &was, cur, &r, fallow, out);
        TagFSLockDrop(&fs->lock);
        if (rc == OK)
            fallow = NULL;
    }
    FallowNodeLetGo(fallow);
    InkRecordLetGo(&r);
    FileRecordRelease(cur);
    return rc;
}

/* A holder of the file's Helm is not waited for: the file is vacated, and the
 * holder answers ERR_FILE_NOT_FOUND at its own Ink, its Stakes given back. */
error_t InkQuitclaim(uint32_t fid, InkReceipt* out)
{
    error_t rc;
    do {
        rc = InkQuitclaimOnce(fid, out);
    } while (rc != OK && InkGoesRound(&rc, INK_MAKES_SPARES));
    if (rc == OK)
        HelmVacate(fid);
    return rc;
}

// ----------------------------------------------------------------------------
// The name is a tag
//
// Every file carries the stem of its name — up to the last dot — as its first
// tag, so "who is called kernel" is a question for the index, not a walk of the
// volume. create puts the tag there and rename moves it, both through this one
// rule. The stem is a key, spelled by the one spelling (tag_spelling.h) but
// never split at ':', and it is read in place: a name is never cut to fit a
// buffer, so it never wears a shorter tag than itself.
// ----------------------------------------------------------------------------

/* The stem of a name as a key. OK with out->key NULL when the name has no
 * stem — ".profile", or nothing but blanks before the dot — and so no tag;
 * ERR_TAGFS_TAG_TOO_LONG for a stem longer than a key may be. */
static error_t InkNameStem(const char* name, TagSpelling* out)
{
    size_t len  = strlen(name);
    size_t stem = len;
    for (size_t i = len; i > 0; i--) {
        if (name[i - 1] == '.') {
            stem = i - 1;
            break;
        }
    }
    error_t rc = TagSpellKey(name, stem, out);
    if (rc == ERR_TAGFS_INVALID_TAG) {
        out->key = NULL;
        return OK;
    }
    return rc;
}

error_t InkNameTagIntern(const char* name, uint16_t* out_tag)
{
    TagSpelling stem;
    *out_tag = TAGFS_INVALID_TAG_ID;
    error_t rc = InkNameStem(name, &stem);
    if (rc != OK || !stem.key)
        return rc;
    rc = TagRegistryIntern(InkState()->registry, &stem, out_tag);
    if (rc == OK)
        rc = TagRegistryFlushFor(InkState()->registry, *out_tag);
    return rc;
}

uint16_t InkNameTagLookup(const char* name)
{
    TagSpelling stem;
    if (!name || InkNameStem(name, &stem) != OK || !stem.key)
        return TAGFS_INVALID_TAG_ID;
    return TagRegistryLookup(InkState()->registry, &stem);
}
