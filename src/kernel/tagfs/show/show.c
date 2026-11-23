#include "show.h"
#include "../file_map/file_map.h"
#include "../ink/ink.h"
#include "../retire/retire.h"
#include "tagfs_volume.h"

/*
 * A program's own tags shown to other programs (tagfs.h): whom it shows them
 * to is the cards of its files — FileRecord.shown_to, 0 for every program —
 * every file of a signed program holding the one list of it, changed by one
 * Ink of each file's fields (InkShown) and read from the card the program is
 * named by. The files that show are a list the file map keeps
 * (FileMapShowing), so a question of whom a program reads is asked of them
 * alone. The asks to be shown are notes in memory (show.h). Inside, a program
 * is who it is (TagOwner); to programs, every list names it by its card.
 */

#define SHOW_BATCH 64u


/* Where (owner, asker) is, or goes, in the notes. Under the lock. */
static uint32_t AskAt(TagFSVolume* vol, uint64_t owner, uint64_t asker)
{
    uint32_t lo = 0, hi = vol->show.asks_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        const ShowAsk* a = &vol->show.asks[mid];
        if (a->owner < owner || (a->owner == owner && a->asker < asker)) lo = mid + 1u;
        else                                                                  hi = mid;
    }
    return lo;
}

/* The note of (owner, asker) — every note of `owner` when `all` — let go. */
static void AsksDrop(TagFSVolume* vol, uint64_t owner, uint64_t asker, bool all)
{
    TagFSLockTake(&vol->show.asks_lock);
    uint32_t from = AskAt(vol, owner, all ? 0 : asker), to = from;
    while (to < vol->show.asks_count && vol->show.asks[to].owner == owner && (all || vol->show.asks[to].asker == asker))
        to++;
    memmove(vol->show.asks + from, vol->show.asks + to, sizeof(ShowAsk) * (vol->show.asks_count - to));
    vol->show.asks_count -= to - from;
    TagFSLockDrop(&vol->show.asks_lock);
}

/* Whether `rec` shows its tags to `reader`: by name, or to every program. */
static bool ShowsTo(const FileRecord* rec, uint64_t reader)
{
    for (uint32_t i = 0; i < rec->shown_count; i++)
        if (rec->shown_to[i] == 0 || rec->shown_to[i] == reader)
            return true;
    return false;
}

/* The files of program `owner` (FileMapProgramFiles), in *files — the
 * caller's to kfree — *n of them, *live whether the card it is named by is
 * not kept deleted. OK, or ERR_NO_MEMORY. Inside the door. */
static error_t ShowFilesOf(TagFSVolume* vol, uint64_t owner, uint64_t** files, uint32_t* n, bool* live)
{
    FileMap* map  = vol->state.files;
    uint64_t card = 0;
    uint32_t want = FileMapProgramFiles(map, owner, NULL, 0, NULL);
    *n     = 0;
    *live  = false;
    *files = want ? kmalloc(sizeof(uint64_t) * want) : NULL;
    if (want && !*files)
        return ERR_NO_MEMORY;
    if (want) {
        uint32_t got = FileMapProgramFiles(map, owner, *files, want, &card);
        *n = got < want ? got : want;
    }
    FileRecord* rec = card ? FileMapRecordHold(map, card, NULL) : NULL;
    *live = rec && !(rec->flags & TAGFS_FILE_DELETED);
    FileRecordRelease(rec);
    return OK;
}

/* The card program `owner` is named by to programs: its file's id, 0 for one
 * not here. Inside the door. */
static uint64_t ShowCardOf(TagFSVolume* vol, uint64_t owner)
{
    uint64_t card = 0;
    if (owner)
        FileMapProgramFiles(vol->state.files, owner, NULL, 0, &card);
    return card;
}

/* That card held — NULL when there is none. Inside the door. */
static FileRecord* ShowCardHold(TagFSVolume* vol, uint64_t owner)
{
    uint64_t card = ShowCardOf(vol, owner);
    return card ? FileMapRecordHold(vol->state.files, card, NULL) : NULL;
}

static int ShowRising(const void* a, const void* b, void* ctx)
{
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    (void)ctx;
    return x < y ? -1 : x > y;
}

/* `ids`, `n` of them, named as programs name programs — 0, every program,
 * stays 0; one gone is dropped — and put in rising order: how many stay. */
static uint32_t ShowNamed(TagFSVolume* vol, uint64_t* ids, uint32_t n)
{
    uint32_t kept = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t card = ids[i] ? ShowCardOf(vol, ids[i]) : 0;
        if (card || !ids[i])
            ids[kept++] = card;
    }
    KSort(ids, kept, sizeof(uint64_t), ShowRising, NULL);
    return kept;
}

/* From `from` on, `room` of `ids` (rising, `n` of them) into a page. */
static void ShowPaged(const uint64_t* ids, uint32_t n, uint64_t from, uint64_t* out, uint32_t room,
                      bool want_total, TagFSPage* page)
{
    uint32_t at = 0;
    while (at < n && ids[at] < from)
        at++;
    for (; at < n; at++) {
        if (page->delivered < room)
            out[page->delivered++] = ids[at];
        else if (!page->next)
            page->next = ids[at];
        if (want_total)
            page->total++;
        else if (page->next)
            break;
    }
}

error_t TagFSShowTags(TagFSVolume* vol, uint64_t program, uint64_t reader, bool show)
{
    if (program == 0 || program == reader)
        return ERR_INVALID_ARGUMENT;
    if (!tagfs_enter(vol))
        return ERR_TAGFS_NOT_INITIALIZED;
    uint64_t* files = NULL;
    uint32_t  n     = 0;
    bool      live  = false;
    error_t   rc    = show && reader && RetireProgramGone(vol, reader) ? ERR_FILE_NOT_FOUND : OK;
    if (rc == OK)
        rc = ShowFilesOf(vol, program, &files, &n, &live);
    if (rc == OK && (n == 0 || (show && !live)))
        rc = ERR_FILE_NOT_FOUND;    /* none of it here, or only in the trash and asked to show more */
    for (uint32_t i = 0; rc == OK && i < n; i++) {
        InkReceipt receipt;
        rc = InkShown(vol, files[i], reader, show, &receipt);
        if (rc == ERR_FILE_NOT_FOUND)
            rc = OK;    /* a file of it let go meanwhile */
    }
    if (rc == OK)
        AsksDrop(vol, program, reader, show && reader == 0);    /* answered: yes, no — or yes to every one */
    kfree(files);
    tagfs_leave(vol);
    return rc;
}

error_t TagFSAskToSee(TagFSVolume* vol, uint64_t asker, uint64_t owner, bool* noted)
{
    *noted = false;
    if (asker == 0 || owner == 0 || asker == owner)
        return ERR_INVALID_ARGUMENT;
    if (!tagfs_enter(vol))
        return ERR_TAGFS_NOT_INITIALIZED;
    error_t     rc  = RetireProgramGone(vol, owner) || RetireProgramGone(vol, asker) ? ERR_FILE_NOT_FOUND : OK;
    FileRecord* rec = rc == OK ? ShowCardHold(vol, owner) : NULL;
    bool        ask = rc == OK && !(rec && ShowsTo(rec, asker));
    FileRecordRelease(rec);
    if (ask) {
        TagFSLockTake(&vol->show.asks_lock);
        uint32_t at = AskAt(vol, owner, asker);
        if (at == vol->show.asks_count || vol->show.asks[at].owner != owner || vol->show.asks[at].asker != asker) {
            if (vol->show.asks_count == vol->show.asks_room) {
                uint32_t room = vol->show.asks_room ? vol->show.asks_room * 2u : 16u;
                ShowAsk* more = room > vol->show.asks_room ? kmalloc(sizeof(ShowAsk) * room) : NULL;
                if (more && vol->show.asks_count)
                    memcpy(more, vol->show.asks, sizeof(ShowAsk) * vol->show.asks_count);
                if (more) {
                    kfree(vol->show.asks);
                    vol->show.asks      = more;
                    vol->show.asks_room = room;
                }
            }
            if (vol->show.asks_count < vol->show.asks_room) {
                memmove(vol->show.asks + at + 1u, vol->show.asks + at, sizeof(ShowAsk) * (vol->show.asks_count - at));
                vol->show.asks[at] = (ShowAsk){ .owner = owner, .asker = asker };
                vol->show.asks_count++;
                *noted = true;
            } else {
                rc = ERR_NO_MEMORY;
            }
        }
        TagFSLockDrop(&vol->show.asks_lock);
    }
    tagfs_leave(vol);
    return rc;
}

error_t TagFSAsksPage(TagFSVolume* vol, uint64_t owner, uint64_t from, uint64_t* out, uint32_t room, bool want_total,
                      TagFSPage* page)
{
    memset(page, 0, sizeof(*page));
    if (room && !out)
        return ERR_INVALID_ARGUMENT;
    if (!tagfs_enter(vol))
        return ERR_TAGFS_NOT_INITIALIZED;
    /* The askers copied off the notes, the notes' lock let go, and named as
     * programs name them: the file map is asked under a leaf lock of its own. */
    uint64_t* askers = NULL;
    uint32_t  n      = 0;
    error_t   rc     = OK;
    TagFSLockTake(&vol->show.asks_lock);
    uint32_t first = AskAt(vol, owner, 0), last = first;
    while (last < vol->show.asks_count && vol->show.asks[last].owner == owner)
        last++;
    if (last > first && (askers = kmalloc(sizeof(uint64_t) * (last - first))) != NULL)
        for (uint32_t at = first; at < last; at++)
            askers[n++] = vol->show.asks[at].asker;
    TagFSLockDrop(&vol->show.asks_lock);
    if (last > first && !askers)
        rc = ERR_NO_MEMORY;
    if (rc == OK)
        ShowPaged(askers, ShowNamed(vol, askers, n), from, out, room, want_total, page);
    kfree(askers);
    tagfs_leave(vol);
    return rc;
}

void ShowMountReset(TagFSVolume* vol)
{
    TagFSLockTake(&vol->show.asks_lock);
    vol->show.asks_count = 0;
    TagFSLockDrop(&vol->show.asks_lock);
}

error_t ShowAsksForgetGone(TagFSVolume* vol)
{
    TagFSLockTake(&vol->show.asks_lock);
    uint32_t n    = vol->show.asks_count;
    ShowAsk* seen = n ? kmalloc(sizeof(ShowAsk) * n) : NULL;
    if (seen)
        memcpy(seen, vol->show.asks, sizeof(ShowAsk) * n);
    TagFSLockDrop(&vol->show.asks_lock);
    if (n && !seen)
        return ERR_NO_MEMORY;
    /* Whether a program left asks the file map, under a leaf lock of its own:
     * looked at off a copy, the notes' lock let go. */
    for (uint32_t i = 0; i < n; i++)
        if (RetireProgramGone(vol, seen[i].owner) || RetireProgramGone(vol, seen[i].asker))
            AsksDrop(vol, seen[i].owner, seen[i].asker, false);
    kfree(seen);
    return OK;
}

bool TagFSShows(TagFSVolume* vol, uint64_t program, uint64_t reader)
{
    if (program == 0 || !tagfs_enter(vol))
        return false;
    FileRecord* rec   = ShowCardHold(vol, program);
    bool        shows = rec && ShowsTo(rec, reader);
    FileRecordRelease(rec);
    tagfs_leave(vol);
    return shows;
}

/* From `from` on, the next files of the map that show — of its list of them,
 * or of every file when the list is short of room — `room` at the most. */
static uint32_t ShowingNext(TagFSVolume* vol, uint64_t from, uint64_t* out, uint32_t room)
{
    FileMap* map = vol->state.files;
    uint32_t n   = FileMapShowing(map, from, out, room);
    return n == UINT32_MAX ? FileMapFiles(map, from, out, room) : n;
}

error_t TagFSShownToPage(TagFSVolume* vol, uint64_t reader, uint64_t from, uint64_t* out, uint32_t room, bool want_total,
                         TagFSPage* page)
{
    memset(page, 0, sizeof(*page));
    if (room && !out)
        return ERR_INVALID_ARGUMENT;
    if (!tagfs_enter(vol))
        return ERR_TAGFS_NOT_INITIALIZED;
    uint64_t batch[SHOW_BATCH];
    uint32_t n;
    for (; (n = ShowingNext(vol, from, batch, SHOW_BATCH)) > 0; from = batch[n - 1u] + 1u) {
        for (uint32_t i = 0; i < n; i++) {
            FileRecord* rec   = FileMapRecordHold(vol->state.files, batch[i], NULL);
            uint64_t    owner = rec ? (rec->passport ? rec->passport : batch[i]) : 0;
            bool        shows = rec && owner != reader && ShowsTo(rec, reader);
            FileRecordRelease(rec);
            if (!shows || ShowCardOf(vol, owner) != batch[i])
                continue;    /* a signed program is named once, by its card */
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
    tagfs_leave(vol);
    return OK;
}

error_t TagFSShownBy(TagFSVolume* vol, uint64_t program, uint64_t** out, uint32_t* count)
{
    *out   = NULL;
    *count = 0;
    if (!tagfs_enter(vol))
        return ERR_TAGFS_NOT_INITIALIZED;
    FileRecord* rec = ShowCardHold(vol, program);
    error_t     rc  = rec ? OK : ERR_FILE_NOT_FOUND;
    uint32_t    n   = rec ? rec->shown_count : 0;
    if (n && !(*out = kmalloc(sizeof(uint64_t) * n)))
        rc = ERR_NO_MEMORY;
    else if (n)
        memcpy(*out, rec->shown_to, sizeof(uint64_t) * n);
    FileRecordRelease(rec);
    if (rc == OK)
        *count = ShowNamed(vol, *out, n);
    tagfs_leave(vol);
    return rc;
}

/* The volume's show part as it begins: what was not nothing. */
void ShowVolumeBegin(TagFSVolume* vol)
{
    vol->show.asks_lock = (TagFSLock)TAGFS_LOCK_INIT("show_asks", LOCK_RANK_LEAF);
}
