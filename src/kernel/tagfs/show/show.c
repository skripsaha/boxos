#include "../tagfs.h"
#include "../file_map/file_map.h"
#include "../ink/ink.h"
#include "../retire/retire.h"

/*
 * A program's own tags shown to other programs (tagfs.h): whom it shows them
 * to is the card of its own file — FileRecord.shown_to, 0 for every program —
 * changed by one Ink of the file's fields (InkShown) and read from the record.
 * The files that show are a list the file map keeps (FileMapShowing), so a
 * question of whom a program reads is asked of them alone.
 */

#define SHOW_BATCH 64u

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
    tagfs_leave();
    return rc;
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
