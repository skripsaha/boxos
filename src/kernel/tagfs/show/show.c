#include "show.h"
#include "../file_map/file_map.h"
#include "../ink/ink.h"
#include "../retire/retire.h"

/*
 * A program's own tags shown to other programs (tagfs.h): whom it shows them
 * to is the card of its own file — FileRecord.shown_to, 0 for every program —
 * changed by one Ink of the file's fields (InkShown) and read from the record.
 * The files that show are a list the file map keeps (FileMapShowing), so a
 * question of whom a program reads is asked of them alone. The asks to be
 * shown are notes in memory (show.h).
 */

#define SHOW_BATCH 64u

/* One ask: `asker` asked `owner` to show it its tags. */
typedef struct {
    uint64_t owner;
    uint64_t asker;
} ShowAsk;

/* The notes, rising by owner and then by asker — a leaf lock: no other TagFS
 * lock is taken while it is held. */
static TagFSLock s_asks_lock = TAGFS_LOCK_INIT("show_asks", LOCK_RANK_LEAF);
static ShowAsk*  s_asks;
static uint32_t  s_asks_count, s_asks_room;

/* Where (owner, asker) is, or goes, in the notes. Under the lock. */
static uint32_t AskAt(uint64_t owner, uint64_t asker)
{
    uint32_t lo = 0, hi = s_asks_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        const ShowAsk* a = &s_asks[mid];
        if (a->owner < owner || (a->owner == owner && a->asker < asker)) lo = mid + 1u;
        else                                                                  hi = mid;
    }
    return lo;
}

/* The note of (owner, asker) — every note of `owner` when `all` — let go. */
static void AsksDrop(uint64_t owner, uint64_t asker, bool all)
{
    TagFSLockTake(&s_asks_lock);
    uint32_t from = AskAt(owner, all ? 0 : asker), to = from;
    while (to < s_asks_count && s_asks[to].owner == owner && (all || s_asks[to].asker == asker))
        to++;
    memmove(s_asks + from, s_asks + to, sizeof(ShowAsk) * (s_asks_count - to));
    s_asks_count -= to - from;
    TagFSLockDrop(&s_asks_lock);
}

/* Whether `rec` shows its tags to `reader`: by its file, or to every
 * program. */
static bool ShowsTo(const FileRecord* rec, uint64_t reader)
{
    for (uint32_t i = 0; i < rec->shown_count; i++)
        if (rec->shown_to[i] == 0 || rec->shown_to[i] == reader)
            return true;
    return false;
}

error_t TagFSShowTags(uint64_t program, uint64_t reader, bool show)
{
    if (program == 0 || program == reader)
        return ERR_INVALID_ARGUMENT;
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;
    InkReceipt receipt;
    error_t    rc = show && reader && RetireFileGone(reader) ? ERR_FILE_NOT_FOUND : OK;
    if (rc == OK)
        rc = InkShown(program, reader, show, &receipt);
    if (rc == OK)
        AsksDrop(program, reader, show && reader == 0);    /* answered: yes, no — or yes to every one */
    tagfs_leave();
    return rc;
}

error_t TagFSAskToSee(uint64_t asker, uint64_t owner, bool* noted)
{
    *noted = false;
    if (asker == 0 || owner == 0 || asker == owner)
        return ERR_INVALID_ARGUMENT;
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;
    error_t     rc  = RetireFileGone(owner) || RetireFileGone(asker) ? ERR_FILE_NOT_FOUND : OK;
    FileRecord* rec = rc == OK ? FileMapRecordHold(tagfs_get_state()->files, owner, NULL) : NULL;
    bool        ask = rc == OK && !(rec && ShowsTo(rec, asker));
    FileRecordRelease(rec);
    if (ask) {
        TagFSLockTake(&s_asks_lock);
        uint32_t at = AskAt(owner, asker);
        if (at == s_asks_count || s_asks[at].owner != owner || s_asks[at].asker != asker) {
            if (s_asks_count == s_asks_room) {
                uint32_t room = s_asks_room ? s_asks_room * 2u : 16u;
                ShowAsk* more = room > s_asks_room ? kmalloc(sizeof(ShowAsk) * room) : NULL;
                if (more && s_asks_count)
                    memcpy(more, s_asks, sizeof(ShowAsk) * s_asks_count);
                if (more) {
                    kfree(s_asks);
                    s_asks      = more;
                    s_asks_room = room;
                }
            }
            if (s_asks_count < s_asks_room) {
                memmove(s_asks + at + 1u, s_asks + at, sizeof(ShowAsk) * (s_asks_count - at));
                s_asks[at] = (ShowAsk){ .owner = owner, .asker = asker };
                s_asks_count++;
                *noted = true;
            } else {
                rc = ERR_NO_MEMORY;
            }
        }
        TagFSLockDrop(&s_asks_lock);
    }
    tagfs_leave();
    return rc;
}

error_t TagFSAsksPage(uint64_t owner, uint64_t from, uint64_t* out, uint32_t room, bool want_total,
                      TagFSPage* page)
{
    memset(page, 0, sizeof(*page));
    if (room && !out)
        return ERR_INVALID_ARGUMENT;
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;
    TagFSLockTake(&s_asks_lock);
    for (uint32_t at = AskAt(owner, from); at < s_asks_count && s_asks[at].owner == owner; at++) {
        if (page->delivered < room)
            out[page->delivered++] = s_asks[at].asker;
        else if (!page->next)
            page->next = s_asks[at].asker;
        if (want_total)
            page->total++;
        else if (page->next)
            break;
    }
    TagFSLockDrop(&s_asks_lock);
    tagfs_leave();
    return OK;
}

void ShowMountReset(void)
{
    TagFSLockTake(&s_asks_lock);
    s_asks_count = 0;
    TagFSLockDrop(&s_asks_lock);
}

error_t ShowAsksForgetGone(void)
{
    TagFSLockTake(&s_asks_lock);
    uint32_t n    = s_asks_count;
    ShowAsk* seen = n ? kmalloc(sizeof(ShowAsk) * n) : NULL;
    if (seen)
        memcpy(seen, s_asks, sizeof(ShowAsk) * n);
    TagFSLockDrop(&s_asks_lock);
    if (n && !seen)
        return ERR_NO_MEMORY;
    /* Whether a program left asks the file map, under a leaf lock of its own:
     * looked at off a copy, the notes' lock let go. */
    for (uint32_t i = 0; i < n; i++)
        if (RetireFileGone(seen[i].owner) || RetireFileGone(seen[i].asker))
            AsksDrop(seen[i].owner, seen[i].asker, false);
    kfree(seen);
    return OK;
}

bool TagFSShows(uint64_t program, uint64_t reader)
{
    if (program == 0 || !tagfs_enter())
        return false;
    FileRecord* rec   = FileMapRecordHold(tagfs_get_state()->files, program, NULL);
    bool        shows = rec && ShowsTo(rec, reader);
    FileRecordRelease(rec);
    tagfs_leave();
    return shows;
}

/* From `from` on, the next files of the map that show — of its list of them,
 * or of every file when the list is short of room — `room` at the most. */
static uint32_t ShowingNext(uint64_t from, uint64_t* out, uint32_t room)
{
    FileMap* map = tagfs_get_state()->files;
    uint32_t n   = FileMapShowing(map, from, out, room);
    return n == UINT32_MAX ? FileMapFiles(map, from, out, room) : n;
}

error_t TagFSShownToPage(uint64_t reader, uint64_t from, uint64_t* out, uint32_t room, bool want_total,
                         TagFSPage* page)
{
    memset(page, 0, sizeof(*page));
    if (room && !out)
        return ERR_INVALID_ARGUMENT;
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;
    uint64_t batch[SHOW_BATCH];
    uint32_t n;
    for (; (n = ShowingNext(from, batch, SHOW_BATCH)) > 0; from = batch[n - 1u] + 1u) {
        for (uint32_t i = 0; i < n; i++) {
            if (batch[i] == reader)
                continue;
            FileRecord* rec   = FileMapRecordHold(tagfs_get_state()->files, batch[i], NULL);
            bool        shows = rec && ShowsTo(rec, reader);
            FileRecordRelease(rec);
            if (!shows)
                continue;
            if (page->delivered < room)
                out[page->delivered++] = batch[i];
            else if (!page->next)
                page->next = batch[i];
            if (want_total)
                page->total++;
            else if (page->next)
                break;
        }
        if (page->next && !want_total)
            break;
    }
    tagfs_leave();
    return OK;
}

error_t TagFSShownBy(uint64_t program, uint64_t* out, uint32_t room, uint32_t* count)
{
    *count = 0;
    if (room && !out)
        return ERR_INVALID_ARGUMENT;
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;
    FileRecord* rec = FileMapRecordHold(tagfs_get_state()->files, program, NULL);
    error_t     rc  = rec ? OK : ERR_FILE_NOT_FOUND;
    if (rec) {
        for (uint32_t i = 0; i < rec->shown_count && i < room; i++)
            out[i] = rec->shown_to[i];
        *count = rec->shown_count;
    }
    FileRecordRelease(rec);
    tagfs_leave();
    return rc;
}
