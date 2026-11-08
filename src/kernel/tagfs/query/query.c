#include "../tagfs.h"
#include "../file_map/file_map.h"
#include "../tag_registry/tag_registry.h"
#include "boxos_decks.h"
#include "klib.h"

/*
 * ‼ WHICH FILES THERE ARE, ASKED A PAGE AT A TIME.
 *
 * Every question of this kind used to be answered into one array as long as a
 * ceiling — every id the volume had issued, the files it held, or
 * TAGFS_MAX_FILES (1024) when neither was known — and a caller whose array was
 * shorter than the answer was given the first part of it as if it were the
 * whole. The questions are pages now: the matches from a file id on, in id
 * order, as many as the caller has room for, and the page says where the rest
 * begin and, when asked, how many there are. Nothing here is sized by the
 * files the volume holds, and no lock is held from one page to the next —
 * a file made or deleted between two pages is on the second or is not, and
 * nothing already delivered is delivered twice.
 */

/* Ids taken out of the map of live files at a time. */
#define QUERY_BATCH 64u

/* One match onto the page: into the room while there is room, and the first
 * past it noted as where the rest begin. False once nothing more is wanted. */
static bool PageTake(TagFSPage* page, uint64_t* out, uint32_t room, bool want_total,
                     uint64_t fid)
{
    if (page->delivered < room) {
        out[page->delivered++] = fid;
    } else if (!page->next) {
        page->next = fid;
        if (!want_total)
            return false;
    }
    if (want_total)
        page->total++;
    return true;
}

/* The trashed and the hidden are left out of an answer unless the question
 * names them — plainly, or through a wildcard of their key: the tags to leave
 * out, and how many. */
static uint32_t QueryHides(const uint32_t* tag_ids, uint32_t tag_count,
                           const TagIdList* groups, uint32_t group_count, uint32_t hide[2])
{
    const WellKnownTags* wk      = tagfs_get_well_known_tags();
    bool                 trashed = false;
    bool                 hidden  = false;
    for (uint32_t i = 0; i < tag_count; i++) {
        trashed |= tag_ids[i] == wk->trashed;
        hidden  |= tag_ids[i] == wk->hidden;
    }
    for (uint32_t g = 0; g < group_count; g++) {
        for (uint32_t k = 0; k < groups[g].count; k++) {
            trashed |= groups[g].ids[k] == wk->trashed;
            hidden  |= groups[g].ids[k] == wk->hidden;
        }
    }
    uint32_t n = 0;
    if (!trashed && wk->trashed != TAGFS_INVALID_TAG_ID)
        hide[n++] = wk->trashed;
    if (!hidden && wk->hidden != TAGFS_INVALID_TAG_ID)
        hide[n++] = wk->hidden;
    return n;
}

// ----------------------------------------------------------------------------
// Every file
// ----------------------------------------------------------------------------

/* The files are the map's that have a record (FileMapRows). Every one is below
 * the id the next create takes: the mount derives that id past every row and
 * record the medium holds (mount/mount.c), so no live file has an id the
 * volume never issued. */
static void ListPageInside(uint64_t from, uint64_t* out, uint32_t room, bool want_total,
                           TagFSPage* page)
{
    TagFSState* fs = tagfs_get_state();
    uint32_t    hide[2];
    uint32_t    hides = QueryHides(NULL, 0, NULL, 0, hide);
    uint64_t    batch[QUERY_BATCH];
    uint64_t    at = from ? from : 1;
    uint32_t    n;

    while ((n = FileMapRows(fs->files, at, batch, QUERY_BATCH)) > 0) {
        for (uint32_t i = 0; i < n; i++) {
            bool hidden = false;
            for (uint32_t h = 0; h < hides && !hidden; h++)
                hidden = TagBitmapHas(fs->bitmap_index, hide[h], batch[i]);
            if (!hidden && !PageTake(page, out, room, want_total, batch[i]))
                return;
        }
        if (batch[n - 1] == UINT64_MAX)
            return;
        at = batch[n - 1] + 1;
    }
}

error_t TagFSListPage(uint64_t from, uint64_t* out, uint32_t room, bool want_total,
                      TagFSPage* page)
{
    memset(page, 0, sizeof(*page));
    if (room && !out)
        return ERR_INVALID_ARGUMENT;
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;
    ListPageInside(from, out, room, want_total, page);
    tagfs_leave();
    return OK;
}

// ----------------------------------------------------------------------------
// The files that wear every tag asked
// ----------------------------------------------------------------------------

/* The asked tags as the index knows them: a plain tag's id into `tag_ids`, a
 * wildcard's ids as one of `groups`. *wearable false when one of them is a tag
 * — or a key — the registry has not: no file wears them all. A wildcard's ids
 * are copies (TagRegistryKeyIds): the registry frees its own array of a key
 * when the key's next tag outgrows it, and that can happen on another core
 * while the question is asked. The groups' ids are the caller's to kfree, as
 * many groups as *group_count says, whatever the answer. */
static error_t QueryResolve(const TagSpelling* tags, uint32_t count, uint32_t* tag_ids,
                            uint32_t* tag_count, TagIdList* groups, uint32_t* group_count,
                            bool* wearable)
{
    TagFSState* fs = tagfs_get_state();
    *tag_count   = 0;
    *group_count = 0;
    *wearable    = false;
    for (uint32_t i = 0; i < count; i++) {
        if (TagSpellingIsWildcard(&tags[i])) {
            TagIdList* grp = &groups[*group_count];
            if (TagRegistryKeyIds(fs->registry, tags[i].key, tags[i].key_len, grp) != OK)
                return ERR_NO_MEMORY;
            if (grp->count == 0)
                return OK;              /* a key no tag has: no file wears one */
            (*group_count)++;
        } else {
            uint32_t tid = TagRegistryLookup(fs->registry, &tags[i]);
            if (tid == TAGFS_INVALID_TAG_ID)
                return OK;              /* a tag the registry has not: no file wears it */
            tag_ids[(*tag_count)++] = tid;
        }
    }
    *wearable = true;
    return OK;
}

static error_t QueryPageInside(const TagSpelling* tags, uint32_t count, uint64_t from,
                               uint64_t* out, uint32_t room, bool want_total, TagFSPage* page)
{
    TagFSState* fs          = tagfs_get_state();
    uint32_t    tag_count   = 0;
    uint32_t    group_count = 0;
    bool        wearable    = false;
    error_t     rc          = OK;

    uint32_t*  tag_ids = kmalloc(sizeof(uint32_t) * count);
    TagIdList* groups  = kmalloc(sizeof(TagIdList) * count);
    if (!tag_ids || !groups) {
        rc = ERR_NO_MEMORY;
        goto out;
    }
    rc = QueryResolve(tags, count, tag_ids, &tag_count, groups, &group_count, &wearable);
    if (rc != OK || !wearable)
        goto out;

    uint32_t hide[2];
    uint32_t hides = QueryHides(tag_ids, tag_count, groups, group_count, hide);
    if (TagBitmapQueryPage(fs->bitmap_index, tag_ids, tag_count, groups, group_count,
                           hide, hides, from, out, room, want_total, page) != 0)
        rc = ERR_TAGFS_NOT_INITIALIZED;
out:
    for (uint32_t g = 0; g < group_count; g++)
        kfree(groups[g].ids);
    kfree(tag_ids);
    kfree(groups);
    return rc;
}

error_t TagFSQueryPage(const TagSpelling* tags, uint32_t count, uint64_t from,
                       uint64_t* out, uint32_t room, bool want_total, TagFSPage* page)
{
    if (count == 0)
        return TagFSListPage(from, out, room, want_total, page);
    memset(page, 0, sizeof(*page));
    if (!tags || (room && !out))
        return ERR_INVALID_ARGUMENT;
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;
    error_t rc = QueryPageInside(tags, count, from, out, room, want_total, page);
    tagfs_leave();
    return rc;
}

// ----------------------------------------------------------------------------
// The tags files wear
// ----------------------------------------------------------------------------

/* One tag's STORAGE_TAG_LIST record after the `*used` bytes of `out`; false
 * when it does not fit the `room` left. */
static bool TagRecordPut(uint8_t* out, uint32_t room, uint32_t* used, const TagSpelling* tag,
                         uint32_t files, bool system)
{
    const uint64_t need = STORAGE_TAG_LISTED_HEAD + (uint64_t)tag->key_len + tag->value_len;
    if (need > room - *used)
        return false;
    uint8_t*       at        = out + *used;
    const uint16_t value_len = (uint16_t)tag->value_len;
    memcpy(at, &files, sizeof(files));
    at[4] = system ? STORAGE_TAG_LISTED_SYSTEM : 0;
    at[5] = (uint8_t)tag->key_len;
    memcpy(at + 6, &value_len, sizeof(value_len));
    memcpy(at + STORAGE_TAG_LISTED_HEAD, tag->key, tag->key_len);
    if (tag->value_len)
        memcpy(at + STORAGE_TAG_LISTED_HEAD + tag->key_len, tag->value, tag->value_len);
    *used += (uint32_t)need;
    return true;
}

/* Each tag the registry holds is put to the index as one more asked tag
 * beside `within`, and the answer's count alone is wanted: how many of the
 * files that wear `within` wear it too. The registry's spellings are its own
 * and live while the volume is entered, so they are copied out before it is
 * left. */
error_t TagFSTagPage(const TagSpelling* within, uint32_t within_count, uint32_t from,
                     uint8_t* out, uint32_t room, uint32_t* used, uint32_t* delivered,
                     uint32_t* next)
{
    *used      = 0;
    *delivered = 0;
    *next      = 0;
    if ((within_count && !within) || (room && !out))
        return ERR_INVALID_ARGUMENT;
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;

    TagFSState* fs          = tagfs_get_state();
    uint32_t    tag_count   = 0;
    uint32_t    group_count = 0;
    bool        wearable    = false;
    error_t     rc          = OK;

    /* The asked tags and room for one more: the tag being counted. */
    uint32_t*  tag_ids = kmalloc(sizeof(uint32_t) * ((size_t)within_count + 1));
    TagIdList* groups  = within_count ? kmalloc(sizeof(TagIdList) * within_count) : NULL;
    if (!tag_ids || (within_count && !groups)) {
        rc = ERR_NO_MEMORY;
        goto out;
    }
    rc = QueryResolve(within, within_count, tag_ids, &tag_count, groups, &group_count, &wearable);
    if (rc != OK || !wearable)
        goto out;                       /* no file wears `within`: none wears a tag among them */

    uint32_t    id = 0;
    TagSpelling tag;
    for (uint32_t at = from; TagRoll(fs->registry, at, UINT32_MAX, &id, &tag); at = id + 1) {
        tag_ids[tag_count] = id;
        uint32_t  hide[2];
        uint32_t  hides = QueryHides(tag_ids, tag_count + 1, groups, group_count, hide);
        TagFSPage page;
        if (TagBitmapQueryPage(fs->bitmap_index, tag_ids, tag_count + 1, groups, group_count,
                               hide, hides, 1, NULL, 0, true, &page) != 0) {
            rc = ERR_TAGFS_NOT_INITIALIZED;
            break;
        }
        if (page.total == 0)
            continue;                   /* a process's, a Touch's, a Bay's — no file's */

        bool system = false;
        (void)TagRegistrySpelling(fs->registry, id, &tag, &system);
        if (!TagRecordPut(out, room, used, &tag, page.total, system)) {
            *next = id;
            if (*delivered == 0)
                rc = ERR_BUFFER_TOO_SMALL;
            break;
        }
        (*delivered)++;
    }
out:
    for (uint32_t g = 0; g < group_count; g++)
        kfree(groups[g].ids);
    kfree(tag_ids);
    kfree(groups);
    tagfs_leave();
    return rc;
}

// ----------------------------------------------------------------------------
// How far the live files are from lying in one run each
// ----------------------------------------------------------------------------

typedef struct {
    uint32_t files;
    uint32_t gaps;
    uint64_t steps;
} FragCount;

/* A file's ground, Strip by Strip in its order: a gap is a DATA Strip that does
 * not begin where the DATA Strip before it ended — a hole lies on no ground, so
 * the walk of ground passes over it, and a read crosses it with no seek. */
static void FragAdd(FragCount* frag, const FileRecord* rec)
{
    if (!(rec->flags & TAGFS_FILE_ACTIVE))
        return;
    StripWalk walk;
    Strip     run;
    uint64_t  blocks = 0, end = 0;
    StripWalkBegin(rec, &walk);
    while (StripWalkNextGround(&walk, &run)) {
        if (blocks > 0 && run.start != end)
            frag->gaps++;
        blocks += run.count;
        end     = run.start + run.count;
    }
    if (blocks > 1)
        frag->steps += blocks - 1;
    frag->files++;
}

#ifdef CONFIG_VOLUME_TESTS
void TagFSFragOfRecord(const FileRecord* rec, uint32_t* gaps, uint64_t* steps)
{
    FragCount frag = { 0, 0, 0 };
    FragAdd(&frag, rec);
    *gaps  = frag.gaps;
    *steps = frag.steps;
}
#endif

/* One file's record, out of the map of live files: every live file has one. */
static void FragOfFile(FragCount* frag, uint64_t fid)
{
    FileRecord* rec = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    if (!rec)
        return;
    FragAdd(frag, rec);
    FileRecordRelease(rec);
}

/*
 * ‼ COUNTED WHERE A READ HAS TO SEEK, NOT BY HOW THE RECORD SPLITS A FILE.
 *
 * The score used to be one less than each file's extent count, summed, and no
 * share of anything, while the tools print it as a percentage: a file written
 * 4 KiB at a time carries an extent per block, laid end to end, and read as
 * shredded though it reads in one run. And the gaps it stands for were sent
 * out as zero, never counted (CW-N9). A gap is a run that does not begin on
 * the block after the one before it ends; the score is the gaps as a share of
 * every step from one of a file's blocks to its next. The files are the live
 * ones a list names, a batch at a time.
 */
uint32_t tagfs_get_fragmentation_score(uint32_t* out_files, uint32_t* out_gaps)
{
    FragCount frag = { 0, 0, 0 };
    if (out_files)
        *out_files = 0;
    if (out_gaps)
        *out_gaps = 0;
    if (!tagfs_enter())
        return 0;

    uint64_t batch[QUERY_BATCH];
    uint64_t at = 1;
    uint32_t n;
    while ((n = FileMapRows(tagfs_get_state()->files, at, batch, QUERY_BATCH)) > 0) {
        for (uint32_t i = 0; i < n; i++)
            FragOfFile(&frag, batch[i]);
        if (batch[n - 1] == UINT64_MAX)
            break;
        at = batch[n - 1] + 1;
    }
    tagfs_leave();

    if (out_files)
        *out_files = frag.files;
    if (out_gaps)
        *out_gaps = frag.gaps;
    return frag.steps ? (uint32_t)(((uint64_t)frag.gaps * 100u + frag.steps - 1) / frag.steps) : 0;
}
