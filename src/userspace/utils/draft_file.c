/* draft_file.c — the volume side. Everything that crosses between the draft in
 * memory and the tagged file it came from lives here: which file the given
 * name and tags actually mean, reading it in whole however many pieces the
 * storage hands over, and putting the bytes back with the tail cut and the
 * write confirmed. It says what happened into the message line and paints
 * nothing itself.
 */

#include "box/memory.h"
#include "box/string.h"
#include "box/file.h"
#include "box/error.h"

#include "draft.h"

/* ---------------------------------------------------------------------------
 * The volume — resolving a name, reading it, writing it back.
 * ------------------------------------------------------------------------- */

void tagline_from(uint32_t fid)
{
    file_info_t info;
    s_tagline[0] = '\0';

    if (fid == 0) { say_str(s_tagline, sizeof(s_tagline), "new"); return; }
    if (file_info(fid, &info) != 0) return;

    for (uint8_t t = 0; t < info.tag_count && t < 5; t++) {
        if (s_tagline[0]) say_str(s_tagline, sizeof(s_tagline), ",");
        say_str(s_tagline, sizeof(s_tagline), info.tags[t].key);
        if (info.tags[t].value[0]) {
            say_str(s_tagline, sizeof(s_tagline), ":");
            say_str(s_tagline, sizeof(s_tagline), info.tags[t].value);
        }
    }
}

char *join_with_commas(const char *const *words, uint32_t count)
{
    size_t total = 1;
    for (uint32_t i = 0; i < count; i++) total += strlen(words[i]) + 1;

    char *list = malloc(total);
    if (!list) return NULL;
    list[0] = '\0';
    for (uint32_t i = 0; i < count; i++) {
        if (i) say_str(list, total, ",");
        say_str(list, total, words[i]);
    }
    return list;
}

/* Which files on the volume answer to this name. With tags, the ask is the
 * name's stem AND every tag — the BoxOS answer to "which notes.txt did you
 * mean" — and the answers are then held to the filename exactly, because a
 * stem is shared by every extension of it. Returns the count and hands back a
 * malloc'd list the caller frees, or a negative -error_t. */
int matches_by_name(const char *name, const char *tags, uint32_t **out_ids)
{
    *out_ids = NULL;

    if (!tags) {
        int n = find_file_by_name(name, NULL, NULL, 0);
        if (n <= 0) return n;
        uint32_t *ids = malloc((size_t)n * sizeof(uint32_t));
        if (!ids) return -ERR_NO_MEMORY;
        n = find_file_by_name(name, ids, NULL, (size_t)n);
        if (n <= 0) { free(ids); return n; }
        *out_ids = ids;
        return n;
    }

    size_t cut = strlen(name);
    for (size_t i = cut; i > 0; i--) {
        if (name[i - 1] == '.') { cut = i - 1; break; }
    }

    char *list = malloc(cut + strlen(tags) + 2);
    if (!list) return -ERR_NO_MEMORY;
    list[0] = '\0';
    if (cut) {
        memcpy(list, name, cut);
        list[cut] = '\0';
        say_str(list, cut + strlen(tags) + 2, ",");
    }
    say_str(list, cut + strlen(tags) + 2, tags);

    uint32_t *ids = NULL;
    int       n   = query_all(list, &ids);
    free(list);
    if (n <= 0) { free(ids); return n; }

    int kept = 0;
    for (int i = 0; i < n; i++) {
        file_info_t info;
        if (file_info(ids[i], &info) != 0) continue;
        if (strcmp(info.filename, name) != 0) continue;
        ids[kept++] = ids[i];
    }
    if (kept == 0) { free(ids); return 0; }
    *out_ids = ids;
    return kept;
}

/* A short read is not an error — it is the storage answering with what it had
 * in hand, so the loop asks again until the whole size is here. */
int load_content(void)
{
    file_info_t info;
    if (file_info(g_fid, &info) != 0) return 0;
    if (info.size == 0) return book_load("", 0);

    char *buf = malloc((size_t)info.size);
    if (!buf) return 0;

    uint64_t got = 0;
    while (got < info.size) {
        int64_t n = fread(g_fid, got, buf + got, (size_t)(info.size - got));
        if (n < 0) { free(buf); return 0; }
        if (n == 0) break;
        got += (uint64_t)n;
    }

    int ok = book_load(buf, (uint32_t)got);
    free(buf);
    return ok;
}

void save_book(void)
{
    uint32_t len   = 0;
    char    *bytes = book_join(&len);
    if (!bytes) { no_memory("the gathered draft"); return; }

    if (g_fid == 0) {
        int made = create(s_name, g_tags);
        if (made < 0) {
            msg_begin();
            msg_add(s_name);
            msg_add(" could not be created");
            say_err(s_msg, sizeof(s_msg), made);
            free(bytes);
            return;
        }
        g_fid = (uint32_t)made;
        tagline_from(g_fid);
    }

    uint64_t off = 0;
    while (off < len) {
        int64_t w = fwrite(g_fid, off, bytes + off, (size_t)(len - off));
        if (w <= 0) {
            msg_begin();
            msg_add(s_name);
            msg_add(" stopped taking bytes after ");
            msg_num((uint32_t)off);
            if (w < 0) say_err(s_msg, sizeof(s_msg), (int)w);
            free(bytes);
            return;
        }
        off += (uint64_t)w;
    }
    free(bytes);

    /* Shrink-only by construction: a draft that GREW has no tail to cut, and
     * the volume says so with ERR_INVALID_ARGUMENT. That is this call
     * succeeding, and treating it as a failure would call every growing save
     * broken. */
    int rc = file_truncate(g_fid, len);
    if (rc != 0 && box_errno_of(rc) != ERR_INVALID_ARGUMENT) {
        msg_begin();
        msg_add(s_name);
        msg_add(" was written, but its old tail could not be cut");
        say_err(s_msg, sizeof(s_msg), rc);
        return;
    }

    /* The bytes left the cabin, so the buffer is no longer ahead of the file
     * whatever the plate says next — but an unconfirmed write is not a safe
     * one, and saying so is the whole point of asking. */
    g_dirty = 0;
    g_saves++;

    if (anchor(g_fid) != 0) {
        msg_begin();
        msg_add(s_name);
        msg_add(" was written, but the volume did not confirm it");
        return;
    }

    msg_begin();
    msg_add(s_name);
    msg_add(" saved - ");
    msg_num(g_book.count);
    msg_add(" lines, ");
    msg_num(len);
    msg_add(" bytes");
}
