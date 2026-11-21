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
 * begin and, when asked, how many there are. Every question is asked in one
 * owner's tags (tagfs.h, TagOwner): a program asking for `save` meets its own
 * `save` and never the person's, and the person never a program's. Nothing here is sized by the
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

/* One more id at the end of a growing array of them; false when the heap
 * would not give the room, the array as it was. */
static bool UnlessAdd(uint32_t** ids, uint32_t* count, uint32_t* room, uint32_t id)
{
    if (*count == *room) {
        const uint32_t grown  = *room ? *room * 2u : 8u;
        uint32_t*      bigger = krealloc(*ids, sizeof(uint32_t) * grown);
        if (!bigger)
            return false;
        *ids  = bigger;
        *room = grown;
    }
    (*ids)[(*count)++] = id;
    return true;
}

/* The ids a question sets aside — each of `owner`'s tags of `without` the
 * registry holds, every one of its key for a wildcard — into one array the caller kfrees
 * (*ids NULL when the question sets none aside). A tag the registry has not
 * is worn by no file, and sets nothing aside. Which tags are set aside is the
 * asker's to say; the volume leaves out only the files it keeps deleted
 * (TAGFS_FILE_DELETED), which `live` adds as the one set more, and which a
 * question for them alone (`live` false) does not. */
static error_t QueryUnless(TagOwner owner, const TagSpelling* without, uint32_t without_count, bool live,
                           uint32_t** ids, uint32_t* count)
{
    TagFSState* fs   = tagfs_get_state();
    uint32_t    room = 0;
    *ids   = NULL;
    *count = 0;
    if (live && !UnlessAdd(ids, count, &room, TAGFS_DELETED_MEMBERS))
        return ERR_NO_MEMORY;
    for (uint32_t i = 0; i < without_count; i++) {
        bool held = true;
        if (TagSpellingIsWildcard(&without[i])) {
            /* A wildcard's ids are copies (TagRegistryKeyIds), as in QueryResolve. */
            TagIdList key;
            if (TagRegistryKeyIds(fs->registry, owner, without[i].key, without[i].key_len, &key) != OK) {
                held = false;
            } else {
                for (uint32_t k = 0; k < key.count && held; k++)
                    held = UnlessAdd(ids, count, &room, key.ids[k]);
                kfree(key.ids);
            }
        } else {
            const uint32_t tid = TagRegistryLookup(fs->registry, owner, &without[i]);
            if (tid != TAGFS_INVALID_TAG_ID)
                held = UnlessAdd(ids, count, &room, tid);
        }
        if (!held) {
            kfree(*ids);
            *ids   = NULL;
            *count = 0;
            return ERR_NO_MEMORY;
        }
    }
    return OK;
}

// ----------------------------------------------------------------------------
// Every file
// ----------------------------------------------------------------------------

/* The files are the map's that have a record (FileMapFiles). Every one is below
 * the id the next create takes: the mount derives that id past every record
 * the book holds (book_mount.c), so no live file has an id the volume never
 * issued. */
static void ListPageInside(const uint32_t* unless, uint32_t unless_count, uint64_t from, uint64_t* out,
                           uint32_t room, bool want_total, TagFSPage* page)
{
    TagFSState* fs = tagfs_get_state();
    uint64_t    batch[QUERY_BATCH];
    uint64_t    at = from ? from : 1;
    uint32_t    n;

    while ((n = FileMapFiles(fs->files, at, batch, QUERY_BATCH)) > 0) {
        for (uint32_t i = 0; i < n; i++) {
            bool aside = false;
            for (uint32_t u = 0; u < unless_count && !aside; u++)
                aside = TagBitmapHas(fs->bitmap_index, unless[u], batch[i]);
            if (!aside && !PageTake(page, out, room, want_total, batch[i]))
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
    const uint32_t aside = TAGFS_DELETED_MEMBERS;
    ListPageInside(&aside, 1, from, out, room, want_total, page);
    tagfs_leave();
    return OK;
}

// ----------------------------------------------------------------------------
// The files that wear every tag asked
// ----------------------------------------------------------------------------

/* `owner`'s asked tags as the index knows them: a plain tag's id into
 * `tag_ids`, a wildcard's ids as one of `groups`. *wearable false when one of them is a tag
 * — or a key — the registry has not: no file wears them all. A wildcard's ids
 * are copies (TagRegistryKeyIds): the registry frees its own array of a key
 * when the key's next tag outgrows it, and that can happen on another core
 * while the question is asked. The groups' ids are the caller's to kfree, as
 * many groups as *group_count says, whatever the answer. */
static error_t QueryResolve(TagOwner owner, const TagSpelling* tags, uint32_t count, uint32_t* tag_ids,
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
            if (TagRegistryKeyIds(fs->registry, owner, tags[i].key, tags[i].key_len, grp) != OK)
                return ERR_NO_MEMORY;
            if (grp->count == 0)
                return OK;              /* a key no tag has: no file wears one */
            (*group_count)++;
        } else {
            uint32_t tid = TagRegistryLookup(fs->registry, owner, &tags[i]);
            if (tid == TAGFS_INVALID_TAG_ID)
                return OK;              /* a tag the registry has not: no file wears it */
            tag_ids[(*tag_count)++] = tid;
        }
    }
    *wearable = true;
    return OK;
}

static error_t QueryPageInside(TagOwner owner, const TagSpelling* tags, uint32_t count,
                               const TagSpelling* without, uint32_t without_count, bool live, uint64_t from,
                               uint64_t* out, uint32_t room, bool want_total, TagFSPage* page)
{
    TagFSState* fs           = tagfs_get_state();
    uint32_t    tag_count    = 0;
    uint32_t    group_count  = 0;
    uint32_t*   unless       = NULL;
    uint32_t    unless_count = 0;
    bool        wearable     = false;
    error_t     rc           = OK;

    uint32_t*  tag_ids = kmalloc(sizeof(uint32_t) * ((size_t)count + 1u));
    TagIdList* groups  = count ? kmalloc(sizeof(TagIdList) * count) : NULL;
    if (!tag_ids || (count && !groups)) {
        rc = ERR_NO_MEMORY;
        goto out;
    }
    rc = QueryResolve(owner, tags, count, tag_ids, &tag_count, groups, &group_count, &wearable);
    if (rc != OK || !wearable)
        goto out;
    if (!live)
        tag_ids[tag_count++] = TAGFS_DELETED_MEMBERS;
    rc = QueryUnless(owner, without, without_count, live, &unless, &unless_count);
    if (rc != OK)
        goto out;

    if (live && count == 0)
        ListPageInside(unless, unless_count, from, out, room, want_total, page);
    else if (TagBitmapQueryPage(fs->bitmap_index, tag_ids, tag_count, groups, group_count,
                                unless, unless_count, from, out, room, want_total, page) != 0)
        rc = ERR_TAGFS_NOT_INITIALIZED;
out:
    for (uint32_t g = 0; g < group_count; g++)
        kfree(groups[g].ids);
    kfree(tag_ids);
    kfree(groups);
    kfree(unless);
    return rc;
}

static error_t QueryPageOf(TagOwner owner, const TagSpelling* tags, uint32_t count, const TagSpelling* without,
                           uint32_t without_count, bool live, uint64_t from, uint64_t* out, uint32_t room,
                           bool want_total, TagFSPage* page)
{
    if (live && count == 0 && without_count == 0)
        return TagFSListPage(from, out, room, want_total, page);
    memset(page, 0, sizeof(*page));
    if ((count && !tags) || (without_count && !without) || (room && !out))
        return ERR_INVALID_ARGUMENT;
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;
    error_t rc = QueryPageInside(owner, tags, count, without, without_count, live, from, out, room, want_total,
                                 page);
    tagfs_leave();
    return rc;
}

error_t TagFSQueryPage(TagOwner owner, const TagSpelling* tags, uint32_t count, const TagSpelling* without,
                       uint32_t without_count, uint64_t from, uint64_t* out, uint32_t room,
                       bool want_total, TagFSPage* page)
{
    return QueryPageOf(owner, tags, count, without, without_count, true, from, out, room, want_total, page);
}

error_t TagFSQueryKeptPage(TagOwner owner, const TagSpelling* tags, uint32_t count, const TagSpelling* without,
                           uint32_t without_count, uint64_t from, uint64_t* out, uint32_t room,
                           bool want_total, TagFSPage* page)
{
    return QueryPageOf(owner, tags, count, without, without_count, false, from, out, room, want_total, page);
}

// ----------------------------------------------------------------------------
// The files called a word
// ----------------------------------------------------------------------------

/* Whether the file wears every tag asked as QueryResolve laid them out: each
 * of `tag_ids`, and one of each group's. */
static bool WearsAsked(const TagFSState* fs, uint64_t fid, const uint32_t* tag_ids, uint32_t tag_count,
                       const TagIdList* groups, uint32_t group_count)
{
    for (uint32_t i = 0; i < tag_count; i++)
        if (!TagBitmapHas(fs->bitmap_index, tag_ids[i], fid))
            return false;
    for (uint32_t g = 0; g < group_count; g++) {
        bool wears = false;
        for (uint32_t k = 0; k < groups[g].count && !wears; k++)
            wears = TagBitmapHas(fs->bitmap_index, groups[g].ids[k], fid);
        if (!wears)
            return false;
    }
    return true;
}

/* The files the index names for the word, a batch at a time from `from` on,
 * each onto the page when it wears the asked tags and none of `unless`. */
static void NamedPageInside(const char* word, size_t length, const uint32_t* tag_ids, uint32_t tag_count,
                            const TagIdList* groups, uint32_t group_count, const uint32_t* unless,
                            uint32_t unless_count, uint64_t from, uint64_t* out, uint32_t room,
                            bool want_total, TagFSPage* page)
{
    TagFSState* fs = tagfs_get_state();
    uint64_t    batch[QUERY_BATCH];
    uint64_t    at = from ? from : 1;
    uint64_t    next;
    do {
        uint32_t n = FileMapNamed(fs->files, word, length, at, batch, QUERY_BATCH, &next);
        for (uint32_t i = 0; i < n; i++) {
            bool aside = false;
            for (uint32_t u = 0; u < unless_count && !aside; u++)
                aside = TagBitmapHas(fs->bitmap_index, unless[u], batch[i]);
            if (!aside && WearsAsked(fs, batch[i], tag_ids, tag_count, groups, group_count) &&
                !PageTake(page, out, room, want_total, batch[i]))
                return;
        }
        at = next;
    } while (next != 0);
}

error_t TagFSNamedPage(TagOwner owner, const char* word, size_t length, const TagSpelling* tags, uint32_t count,
                       const TagSpelling* without, uint32_t without_count, uint64_t from, uint64_t* out,
                       uint32_t room, bool want_total, TagFSPage* page)
{
    memset(page, 0, sizeof(*page));
    if (!word || length == 0 || (count && !tags) || (without_count && !without) || (room && !out))
        return ERR_INVALID_ARGUMENT;
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;

    uint32_t    tag_count    = 0;
    uint32_t    group_count  = 0;
    uint32_t*   unless       = NULL;
    uint32_t    unless_count = 0;
    bool        wearable     = false;
    error_t     rc           = OK;
    uint32_t*   tag_ids      = kmalloc(sizeof(uint32_t) * ((size_t)count + 1u));
    TagIdList*  groups       = count ? kmalloc(sizeof(TagIdList) * count) : NULL;
    if (!tag_ids || (count && !groups))
        rc = ERR_NO_MEMORY;
    else
        rc = QueryResolve(owner, tags, count, tag_ids, &tag_count, groups, &group_count, &wearable);
    if (rc == OK && wearable)
        rc = QueryUnless(owner, without, without_count, false, &unless, &unless_count);
    if (rc == OK && wearable)
        NamedPageInside(word, length, tag_ids, tag_count, groups, group_count, unless, unless_count, from, out,
                        room, want_total, page);
    for (uint32_t g = 0; g < group_count; g++)
        kfree(groups[g].ids);
    kfree(tag_ids);
    kfree(groups);
    kfree(unless);
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

/* Each of `owner`'s tags the registry holds is put to the index as one more
 * asked tag beside `within`, and the answer's count alone is wanted: how many
 * of the files that wear `within` wear it too. Another owner's tag is passed
 * over whatever files wear it — the person's list is the person's tags, and a
 * program's its own. The registry's spellings are its own and live while the
 * volume is entered, so they are copied out before it is left. */
error_t TagFSTagPage(TagOwner owner, const TagSpelling* within, uint32_t within_count, const TagSpelling* without,
                     uint32_t without_count, uint32_t from, uint8_t* out, uint32_t room, uint32_t* used,
                     uint32_t* delivered, uint32_t* next)
{
    *used      = 0;
    *delivered = 0;
    *next      = 0;
    if ((within_count && !within) || (without_count && !without) || (room && !out))
        return ERR_INVALID_ARGUMENT;
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;

    TagFSState* fs           = tagfs_get_state();
    uint32_t    tag_count    = 0;
    uint32_t    group_count  = 0;
    uint32_t*   unless       = NULL;
    uint32_t    unless_count = 0;
    bool        wearable     = false;
    error_t     rc           = OK;

    /* The asked tags and room for one more: the tag being counted. */
    uint32_t*  tag_ids = kmalloc(sizeof(uint32_t) * ((size_t)within_count + 1));
    TagIdList* groups  = within_count ? kmalloc(sizeof(TagIdList) * within_count) : NULL;
    if (!tag_ids || (within_count && !groups)) {
        rc = ERR_NO_MEMORY;
        goto out;
    }
    rc = QueryResolve(owner, within, within_count, tag_ids, &tag_count, groups, &group_count, &wearable);
    if (rc != OK || !wearable)
        goto out;                       /* no file wears `within`: none wears a tag among them */
    rc = QueryUnless(owner, without, without_count, true, &unless, &unless_count);
    if (rc != OK)
        goto out;

    uint32_t    id = 0;
    TagSpelling tag;
    TagOwner    whose;
    for (uint32_t at = from; TagRoll(fs->registry, at, UINT32_MAX, &id, &tag, &whose); at = id + 1) {
        if (!TagOwnerSame(whose, owner))
            continue;
        tag_ids[tag_count] = id;
        TagFSPage page;
        if (TagBitmapQueryPage(fs->bitmap_index, tag_ids, tag_count + 1, groups, group_count,
                               unless, unless_count, 1, NULL, 0, true, &page) != 0) {
            rc = ERR_TAGFS_NOT_INITIALIZED;
            break;
        }
        if (page.total == 0)
            continue;                   /* a process's, a Touch's, a Bay's — no file's */

        bool system = false;
        (void)TagRegistrySpelling(fs->registry, id, &tag, &system, NULL);
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
    kfree(unless);
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

/* A file's own ground, Strip by Strip in its order: a gap is a DATA Strip that
 * does not begin where the DATA Strip before it ended — a hole lies on no
 * ground, so the walk of ground passes over it, and a read crosses it with no
 * seek. A SHARED Strip lies where the original it was laid on lies (dedup.h):
 * the seek onto it and off it is what sharing costs, and no defrag takes it
 * back (plot.h, PlotStretchScattered) — so it ends a stretch of the file's
 * own runs, and gaps and steps are counted inside the stretches alone. */
static void FragAdd(FragCount* frag, const FileRecord* rec)
{
    if (!(rec->flags & TAGFS_FILE_ACTIVE))
        return;
    StripWalk walk;
    Strip     run;
    uint64_t  blocks = 0, end = 0;     /* of the stretch under way */
    StripWalkBegin(rec, &walk);
    while (StripWalkNextGround(&walk, &run)) {
        if (run.kind == STRIP_SHARED) {
            frag->steps += blocks > 1 ? blocks - 1 : 0;
            blocks = 0;
            continue;
        }
        if (blocks > 0 && run.start != end)
            frag->gaps++;
        blocks += run.count;
        end     = run.start + run.count;
    }
    frag->steps += blocks > 1 ? blocks - 1 : 0;
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
    while ((n = FileMapFiles(tagfs_get_state()->files, at, batch, QUERY_BATCH)) > 0) {
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
