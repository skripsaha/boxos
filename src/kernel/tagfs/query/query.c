#include "../tagfs.h"
#include "../file_map/file_map.h"
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
static bool PageTake(TagFSPage* page, uint32_t* out, uint32_t room, bool want_total,
                     uint32_t fid)
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
static uint32_t QueryHides(const uint16_t* tag_ids, uint32_t tag_count,
                           const TagIdList* groups, uint32_t group_count, uint16_t hide[2])
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

/* The files are the map's that have a row. Every one is below the id the next
 * create takes: the mount derives that id past every row and record the
 * medium holds (mount/mount.c), so no row names an id the volume never
 * issued. */
static void ListPageInside(uint32_t from, uint32_t* out, uint32_t room, bool want_total,
                           TagFSPage* page)
{
    TagFSState* fs = tagfs_get_state();
    uint16_t    hide[2];
    uint32_t    hides = QueryHides(NULL, 0, NULL, 0, hide);
    uint32_t    batch[QUERY_BATCH];
    uint32_t    at = from ? from : 1;
    uint32_t    n;

    while ((n = FileMapRows(fs->files, at, batch, QUERY_BATCH)) > 0) {
        for (uint32_t i = 0; i < n; i++) {
            bool hidden = false;
            for (uint32_t h = 0; h < hides && !hidden; h++)
                hidden = TagBitmapHas(fs->bitmap_index, hide[h], batch[i]);
            if (!hidden && !PageTake(page, out, room, want_total, batch[i]))
                return;
        }
        if (batch[n - 1] == 0xFFFFFFFFu)
            return;
        at = batch[n - 1] + 1;
    }
}

error_t TagFSListPage(uint32_t from, uint32_t* out, uint32_t room, bool want_total,
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

static error_t QueryPageInside(const TagSpelling* tags, uint32_t count, uint32_t from,
                               uint32_t* out, uint32_t room, bool want_total, TagFSPage* page)
{
    TagFSState* fs          = tagfs_get_state();
    uint32_t    tag_count   = 0;
    uint32_t    group_count = 0;
    error_t     rc          = OK;

    /* A wildcard's ids are copies (TagRegistryKeyIds): the registry frees its
     * own array of a key when the key's next tag outgrows it, and that can
     * happen on another core while this query reads. */
    uint16_t*  tag_ids = kmalloc(sizeof(uint16_t) * count);
    TagIdList* groups  = kmalloc(sizeof(TagIdList) * count);
    if (!tag_ids || !groups) {
        rc = ERR_NO_MEMORY;
        goto out;
    }

    for (uint32_t i = 0; i < count; i++) {
        if (TagSpellingIsWildcard(&tags[i])) {
            TagIdList* grp = &groups[group_count];
            if (TagRegistryKeyIds(fs->registry, tags[i].key, tags[i].key_len, grp) != OK) {
                rc = ERR_NO_MEMORY;
                goto out;
            }
            if (grp->count == 0)
                goto out;               /* a key no tag has: no file wears one */
            group_count++;
        } else {
            uint16_t tid = TagRegistryLookup(fs->registry, &tags[i]);
            if (tid == TAGFS_INVALID_TAG_ID)
                goto out;               /* a tag the registry has not: no file wears it */
            tag_ids[tag_count++] = tid;
        }
    }

    uint16_t hide[2];
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

error_t TagFSQueryPage(const TagSpelling* tags, uint32_t count, uint32_t from,
                       uint32_t* out, uint32_t room, bool want_total, TagFSPage* page)
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
// How far the live files are from lying in one run each
// ----------------------------------------------------------------------------

typedef struct {
    uint32_t files;
    uint32_t gaps;
    uint32_t steps;
} FragCount;

static void FragAdd(FragCount* frag, uint32_t flags, const FileExtent* extents, uint32_t count)
{
    if (!(flags & TAGFS_FILE_ACTIVE))
        return;
    uint32_t blocks = 0;
    for (uint32_t e = 0; extents && e < count; e++) {
        if (e > 0 && extents[e].start_block != extents[e - 1].start_block + extents[e - 1].block_count)
            frag->gaps++;
        blocks += extents[e].block_count;
    }
    if (blocks > 1)
        frag->steps += blocks - 1;
    frag->files++;
}

/* One file's record, out of the map of live files: every live file has one. */
static void FragOfFile(FragCount* frag, uint32_t fid)
{
    FileRecord* rec = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    if (!rec)
        return;
    FragAdd(frag, rec->flags, rec->extents, rec->extent_count);
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

    uint32_t batch[QUERY_BATCH];
    uint32_t at = 1;
    uint32_t n;
    while ((n = FileMapRows(tagfs_get_state()->files, at, batch, QUERY_BATCH)) > 0) {
        for (uint32_t i = 0; i < n; i++)
            FragOfFile(&frag, batch[i]);
        if (batch[n - 1] == 0xFFFFFFFFu)
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
