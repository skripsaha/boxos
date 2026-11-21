/*
 * file.c — userspace file/tag wrappers (Phase 12: Manifest-only).
 *
 * Storage Deck is now fully Manifest-native. Every wrapper builds a 1-op
 * Manifest via MfCall1 with the new param/crate layout — the legacy
 * 168/176-byte payload caps are gone. fread / fwrite now transfer up to the
 * caller's buffer size in a single syscall.
 */

#include "box/file.h"
#include "box/core/manifest.h"
#include "box/core/notify.h"
#include "box/core/result.h"
#include "box/string.h"
#include "box/types.h"
#include "box/error.h"
#include "box/memory.h"     /* malloc / free — a crate sized to the ask */
#include "box/debug.h"      /* kdbg_print — what this ABI cuts is said out loud */
#include "box/timeouts.h"   /* BOX_ANSWER_GUARANTEED */
#include "boxos_decks.h"    /* STORAGE_* opcodes + STORAGE_SCOPE_* — single source */

/* =========================================================================
 *  CREATE / QUERY — inside the Use Context, or everywhere
 * ========================================================================= */

static int64_t create_scoped(const char *filename, const char *tags, uint8_t scope)
{
    if (!filename || filename[0] == '\0') return -ERR_INVALID_ARGUMENT;
    size_t fn_len = strlen(filename);
    if (fn_len >= 32) return -ERR_INVALID_ARGUMENT;

    /* params: 32 bytes of NUL-padded filename, then the scope byte. */
    uint8_t params[33] = {0};
    memcpy(params, filename, fn_len);
    params[32] = scope;

    /* in_crate: tag list (optional). */
    const void *in = NULL;
    uint32_t    in_size = 0;
    if (tags && tags[0] != '\0') {
        in = tags;
        in_size = (uint32_t)strlen(tags);
    }

    file_id_t file_id = 0;
    int rc = MfCall1(DECK_STORAGE, STORAGE_OBJ_CREATE,
                     params, sizeof(params),
                     in, in_size,
                     &file_id, sizeof(file_id), NULL,
                     BOX_ANSWER_GUARANTEED, NULL);
    if (rc != 0) return box_fail(rc);
    return (int64_t)file_id;
}

int64_t create(const char *filename, const char *tags)
{
    return create_scoped(filename, tags, STORAGE_SCOPE_USE);
}

int64_t create_everywhere(const char *filename, const char *tags)
{
    return create_scoped(filename, tags, STORAGE_SCOPE_EVERYWHERE);
}

/* The kernel answers [u32 delivered][u32 total][u64 ids[delivered]]: how many
 * it handed over, and how many there are. The crate is sized to the caller's
 * room, not to a buffer of ours — what stood here was a 1 KiB stack buffer
 * that quietly cut every ask down to 254 files. */
/* The params of a question: what comes first, then the tags it sets aside,
 * as storage.query and storage.tag.list take them — a heap block the caller
 * frees, *size its bytes. NULL when there is no memory for it, or when the
 * list would not fit a manifest op's params. */
static uint8_t *question_params(const void *head, uint32_t head_size, const char *without,
                                uint16_t *size)
{
    const size_t aside = without ? strlen(without) : 0;
    if (head_size + aside > UINT16_MAX) return NULL;
    uint8_t *params = malloc(head_size + aside);
    if (!params) return NULL;
    memcpy(params, head, head_size);
    if (aside) memcpy(params + head_size, without, aside);
    *size = (uint16_t)(head_size + aside);
    return params;
}

/* One question of the storage deck that answers file ids: the tag query, whose
 * text is the tags asked, or the name query, whose text is the word and, after
 * a NUL, the tags the files called so must wear. */
typedef struct {
    uint8_t     op;
    const char *text;       /* the tags, or the word; NULL or "" asks no tags */
    const char *without;    /* tags set aside; NULL or "" for none */
    uint8_t     scope;
    const char *tags;       /* the name query's: tags worn too; NULL or "" for none */
} Question;

static int ask_files(const Question *question, file_id_t *file_ids, size_t max_files, uint32_t *out_total)
{
    const char *without = question->without;
    const char *text    = question->text;
    uint8_t     scope   = question->scope;
    if (max_files > 0 && !file_ids) return -ERR_INVALID_ARGUMENT;
    if (max_files > (UINT32_MAX - 8u) / sizeof(file_id_t)) return -ERR_INVALID_ARGUMENT;

    uint16_t params_size = 0;
    uint8_t *params = question_params(&scope, sizeof(scope), without, &params_size);
    if (!params) return without && strlen(without) >= UINT16_MAX ? -ERR_INVALID_ARGUMENT : -ERR_NO_MEMORY;

    uint32_t out_cap = (uint32_t)(8 + max_files * sizeof(file_id_t));
    uint8_t *out = malloc(out_cap);
    if (!out) { free(params); return -ERR_NO_MEMORY; }

    const void *in = NULL;
    uint32_t    in_size = 0;
    char       *joined  = NULL;
    if (text && text[0] != '\0') {
        in = text;
        in_size = (uint32_t)strlen(text);
    }
    if (in && question->tags && question->tags[0] != '\0') {
        size_t tags_len = strlen(question->tags);
        joined = malloc(in_size + 1u + tags_len);
        if (!joined) { free(params); free(out); return -ERR_NO_MEMORY; }
        memcpy(joined, text, in_size);
        joined[in_size] = '\0';
        memcpy(joined + in_size + 1u, question->tags, tags_len);
        in = joined;
        in_size += 1u + (uint32_t)tags_len;
    }

    uint32_t out_actual = 0;
    int rc = MfCall1(DECK_STORAGE, question->op,
                     params, params_size,
                     in, in_size,
                     out, out_cap, &out_actual,
                     BOX_ANSWER_GUARANTEED, NULL);
    free(params);
    free(joined);
    if (rc != 0) { free(out); return box_fail(rc); }
    if (out_actual < 8) { free(out); return -ERR_INTERNAL; }

    uint32_t delivered, total;
    memcpy(&delivered, out,     4);
    memcpy(&total,     out + 4, 4);
    if (delivered > max_files) delivered = (uint32_t)max_files;
    if (delivered > 0) memcpy(file_ids, out + 8, delivered * sizeof(file_id_t));
    free(out);
    if (out_total) *out_total = total;
    return (int)delivered;
}

/* A page of storage.tag.list: room for many tags at a time, and for any one
 * of them — a tag's whole text is narrower than a block of the registry. */
#define TAG_LIST_PAGE (16u * 1024u)

/* The bytes of the records of a page that are whole and inside it, and the
 * text their tags will take as "key:value" strings. 0 when a record is cut. */
static size_t tag_list_page_bytes(const uint8_t *page, uint32_t delivered, size_t *text)
{
    size_t at = STORAGE_TAG_LIST_HEAD;
    for (uint32_t i = 0; i < delivered; i++) {
        if (at + STORAGE_TAG_LISTED_HEAD > TAG_LIST_PAGE) return 0;
        uint16_t value_len;
        memcpy(&value_len, page + at + 6, sizeof(value_len));
        size_t one = STORAGE_TAG_LISTED_HEAD + page[at + 5] + (size_t)value_len;
        if (at + one > TAG_LIST_PAGE) return 0;
        *text += page[at + 5] + (value_len ? 1u + value_len : 0u) + 1u;
        at += one;
    }
    return at - STORAGE_TAG_LIST_HEAD;
}

/* Every page asked for in turn, its records gathered; then the list and the
 * text laid out in one block. */
static int tag_list_scoped(uint8_t scope, const char *without, TagListed **out, size_t *count)
{
    if (!out || !count) return -ERR_INVALID_ARGUMENT;
    *out   = NULL;
    *count = 0;

    uint8_t *page = malloc(TAG_LIST_PAGE);
    if (!page) return -ERR_NO_MEMORY;

    uint8_t *records = NULL;
    size_t   bytes = 0, listed = 0, text = 0;
    uint32_t from = 0;
    int      rc = 0;
    for (;;) {
        uint8_t head[5];
        memcpy(head, &from, sizeof(from));
        head[4] = scope;
        uint16_t params_size = 0;
        uint8_t *params = question_params(head, sizeof(head), without, &params_size);
        if (!params) { rc = -ERR_NO_MEMORY; break; }
        rc = MfCall1(DECK_STORAGE, STORAGE_TAG_LIST, params, params_size, NULL, 0,
                     page, TAG_LIST_PAGE, NULL, BOX_ANSWER_GUARANTEED, NULL);
        free(params);
        if (rc != 0) { rc = box_fail(rc); break; }

        uint32_t delivered, next;
        memcpy(&delivered, page, sizeof(delivered));
        memcpy(&next, page + 4, sizeof(next));
        size_t got = tag_list_page_bytes(page, delivered, &text);
        /* A page cut short, or one that gave nothing and still says there is
         * more from where it was asked, is no answer to go on from. */
        if ((delivered > 0 && got == 0) || (next != 0 && (delivered == 0 || next <= from))) {
            rc = -ERR_INTERNAL;
            break;
        }
        if (got > 0) {
            uint8_t *grown = realloc(records, bytes + got);
            if (!grown) { rc = -ERR_NO_MEMORY; break; }
            records = grown;
            memcpy(records + bytes, page + STORAGE_TAG_LIST_HEAD, got);
            bytes  += got;
            listed += delivered;
        }
        if (next == 0) break;
        from = next;
    }
    free(page);
    if (rc < 0 || listed == 0) {
        free(records);
        return rc;
    }

    TagListed *list = malloc(listed * sizeof(TagListed) + text);
    if (!list) {
        free(records);
        return -ERR_NO_MEMORY;
    }
    char  *pen = (char *)(list + listed);
    size_t at  = 0;
    for (size_t i = 0; i < listed; i++) {
        const uint8_t *record = records + at;
        uint16_t       value_len;
        memcpy(&list[i].Files, record, sizeof(list[i].Files));
        memcpy(&value_len, record + 6, sizeof(value_len));
        list[i].System = (record[4] & STORAGE_TAG_LISTED_SYSTEM) != 0;
        list[i].Tag    = pen;
        memcpy(pen, record + STORAGE_TAG_LISTED_HEAD, record[5]);
        pen += record[5];
        if (value_len) {
            *pen++ = ':';
            memcpy(pen, record + STORAGE_TAG_LISTED_HEAD + record[5], value_len);
            pen += value_len;
        }
        *pen++ = '\0';
        at += STORAGE_TAG_LISTED_HEAD + record[5] + (size_t)value_len;
    }
    free(records);
    *out   = list;
    *count = listed;
    return 0;
}

int tag_list(const char *without, TagListed **out, size_t *count)
{
    return tag_list_scoped(STORAGE_SCOPE_USE, without, out, count);
}

int tag_list_everywhere(const char *without, TagListed **out, size_t *count)
{
    return tag_list_scoped(STORAGE_SCOPE_EVERYWHERE, without, out, count);
}

int query(const char *tags, const char *without, file_id_t *file_ids, size_t max_files)
{
    Question question = { STORAGE_TAG_QUERY, tags, without, STORAGE_SCOPE_USE, NULL };
    return ask_files(&question, file_ids, max_files, NULL);
}

int query_everywhere(const char *tags, const char *without, file_id_t *file_ids, size_t max_files)
{
    Question question = { STORAGE_TAG_QUERY, tags, without, STORAGE_SCOPE_EVERYWHERE, NULL };
    return ask_files(&question, file_ids, max_files, NULL);
}

/* Every match, however many: ask how many there are, then ask for that many.
 * The volume may grow between the two asks — then the second answer says so,
 * and the ask is repeated with room for the new count. */
static int ask_all(const Question *question, file_id_t **out_ids)
{
    if (!out_ids) return -ERR_INVALID_ARGUMENT;
    *out_ids = NULL;

    uint32_t total = 0;
    int rc = ask_files(question, NULL, 0, &total);
    if (rc < 0) return rc;

    for (;;) {
        if (total == 0) return 0;
        file_id_t *ids = malloc(total * sizeof(file_id_t));
        if (!ids) return -ERR_NO_MEMORY;
        uint32_t now = 0;
        rc = ask_files(question, ids, total, &now);
        if (rc < 0) { free(ids); return rc; }
        if (now <= total) { *out_ids = ids; return rc; }
        free(ids);
        total = now;
    }
}

int query_all(const char *tags, const char *without, file_id_t **out_ids)
{
    Question question = { STORAGE_TAG_QUERY, tags, without, STORAGE_SCOPE_USE, NULL };
    return ask_all(&question, out_ids);
}

int query_all_everywhere(const char *tags, const char *without, file_id_t **out_ids)
{
    Question question = { STORAGE_TAG_QUERY, tags, without, STORAGE_SCOPE_EVERYWHERE, NULL };
    return ask_all(&question, out_ids);
}

int query_deleted(const char *tags, const char *without, file_id_t *file_ids, size_t max_files)
{
    Question question = { STORAGE_TAG_QUERY, tags, without, STORAGE_SCOPE_DELETED, NULL };
    return ask_files(&question, file_ids, max_files, NULL);
}

int query_all_deleted(const char *tags, const char *without, file_id_t **out_ids)
{
    Question question = { STORAGE_TAG_QUERY, tags, without, STORAGE_SCOPE_DELETED, NULL };
    return ask_all(&question, out_ids);
}

int query_name_all(const char *word, const char *tags, const char *without, file_id_t **out_ids)
{
    if (!word || word[0] == '\0') return -ERR_INVALID_ARGUMENT;
    Question question = { STORAGE_NAME_QUERY, word, without, STORAGE_SCOPE_USE, tags };
    return ask_all(&question, out_ids);
}

int query_name_all_everywhere(const char *word, const char *tags, const char *without, file_id_t **out_ids)
{
    if (!word || word[0] == '\0') return -ERR_INVALID_ARGUMENT;
    Question question = { STORAGE_NAME_QUERY, word, without, STORAGE_SCOPE_EVERYWHERE, tags };
    return ask_all(&question, out_ids);
}

int query_word_all(const char *word, file_id_t **out_ids)
{
    if (!out_ids) return -ERR_INVALID_ARGUMENT;
    *out_ids = NULL;
    if (!word || word[0] == '\0') return -ERR_INVALID_ARGUMENT;

    file_id_t *worn = NULL;
    int worn_count = query_all(word, NULL, &worn);
    if (worn_count < 0) return worn_count;
    file_id_t *called = NULL;
    int called_count = query_name_all(word, NULL, NULL, &called);
    if (called_count < 0) { free(worn); return called_count; }

    file_id_t *both = malloc(((size_t)worn_count + (size_t)called_count + 1u) * sizeof(file_id_t));
    if (!both) { free(worn); free(called); return -ERR_NO_MEMORY; }
    int a = 0, b = 0, n = 0;
    while (a < worn_count || b < called_count) {
        if (b == called_count || (a < worn_count && worn[a] < called[b])) both[n++] = worn[a++];
        else if (a == worn_count || called[b] < worn[a])                  both[n++] = called[b++];
        else { both[n++] = worn[a++]; b++; }
    }
    free(worn);
    free(called);
    if (n == 0) { free(both); return 0; }
    *out_ids = both;
    return n;
}

/* =========================================================================
 *  FILE_INFO — parse the variable-length blob ObjGetInfo writes.
 *
 *  ‼ WHAT IS CUT HERE IS SAID OUT LOUD.
 *
 *  Nothing on the volume is cut. TagFS keeps a file name and every tag at its
 *  own length, the image builder interns the whole name, and the deck answers
 *  with a self-describing blob of up to a kilobyte. The cutting belongs to
 *  this library alone: file_info_t is a fixed-size structure — 32 bytes of
 *  name, five tags of key[11] and value[12] — and it used to cut in silence.
 *  That is how `files` came to print the tag "anchor_tes" for a file tagged
 *  "anchor_test": type `files anchor_tes` and the answer is "No files found",
 *  because the name the machine showed is one the volume has never heard. On a
 *  machine where a tag IS the way to reach a thing, that is the main interface
 *  refusing, and neither the screen nor the log said why.
 *
 *  Widening the structure is the owner's decision. Saying what it costs is
 *  not, and it is said once per kind per program: one listing of a 69-file
 *  volume would otherwise bury the log it is meant to explain.
 * ========================================================================= */

int file_info(file_id_t file_id, file_info_t *info)
{
    /* One line each, the first time this program meets that kind of cut. */
    static uint8_t said_name  = 0;
    static uint8_t said_count = 0;
    static uint8_t said_text  = 0;

    if (!info) return -ERR_INVALID_ARGUMENT;

    uint8_t  out[1024];
    uint32_t out_actual = 0;
    int rc = MfCall1(DECK_STORAGE, STORAGE_OBJ_GET_INFO,
                     &file_id, sizeof(file_id),
                     NULL, 0,
                     out, sizeof(out), &out_actual,
                     BOX_ANSWER_GUARANTEED, NULL);
    if (rc != 0) return box_fail(rc);
    if (out_actual < 22) return -ERR_INTERNAL;

    memset(info, 0, sizeof(*info));

    size_t pos = 0;
    memcpy(&info->file_id, out + pos, 8); pos += 8;
    memcpy(&info->size,    out + pos, 8); pos += 8;
    uint32_t tag_count_raw = 0;
    uint16_t fn_len        = 0;
    memcpy(&tag_count_raw, out + pos, 4); pos += 4;
    memcpy(&fn_len,        out + pos, 2); pos += 2;

    if (pos + fn_len > out_actual) return -ERR_INTERNAL;
    size_t fn_copy = fn_len < sizeof(info->filename) - 1 ? fn_len
                                                          : sizeof(info->filename) - 1;
    memcpy(info->filename, out + pos, fn_copy);
    info->filename[fn_copy] = '\0';
    pos += fn_len;

    if (fn_len > fn_copy && !__atomic_exchange_n(&said_name, 1, __ATOMIC_RELAXED))
        kdbg_print("[file] file %lu is named with %u bytes on the volume and "
                   "file_info_t holds %u — what this program shows is a prefix, "
                   "and a prefix is not a name anybody can type back. Said once.",
                   file_id, (unsigned)fn_len,
                   (unsigned)(sizeof(info->filename) - 1));

    const size_t slots = sizeof(info->tags) / sizeof(info->tags[0]);
    info->tag_count = (uint8_t)(tag_count_raw > slots ? slots : tag_count_raw);

    if (tag_count_raw > slots && !__atomic_exchange_n(&said_count, 1, __ATOMIC_RELAXED))
        kdbg_print("[file] file %lu wears %u tags and file_info_t carries %u — the "
                   "tags this program lists are not all of them, and nothing in the "
                   "list says so. Said once.",
                   file_id, (unsigned)tag_count_raw, (unsigned)slots);

    for (uint32_t i = 0; i < tag_count_raw; i++) {
        if (pos + 5 > out_actual) break;
        uint16_t kl = 0, vl = 0;
        memcpy(&kl, out + pos, 2); pos += 2;
        memcpy(&vl, out + pos, 2); pos += 2;
        uint8_t type = out[pos]; pos += 1;   /* 1 = system, 0 = user */
        if (pos + kl + vl > out_actual) break;

        if (i < slots) {
            size_t kc = kl < sizeof(info->tags[i].key) - 1 ? kl
                                                            : sizeof(info->tags[i].key) - 1;
            size_t vc = vl < sizeof(info->tags[i].value) - 1 ? vl
                                                              : sizeof(info->tags[i].value) - 1;
            memcpy(info->tags[i].key,   out + pos,       kc);
            info->tags[i].key[kc] = '\0';
            memcpy(info->tags[i].value, out + pos + kl,  vc);
            info->tags[i].value[vc] = '\0';
            info->tags[i].type = type;

            if ((kl > kc || vl > vc) &&
                !__atomic_exchange_n(&said_text, 1, __ATOMIC_RELAXED))
                kdbg_print("[file] a tag of file %lu does not fit file_info_t (key %u "
                           "bytes into %u, value %u into %u) — the tag shown is a "
                           "prefix, and querying that prefix finds nothing. Said once.",
                           file_id, (unsigned)kl,
                           (unsigned)(sizeof(info->tags[i].key) - 1),
                           (unsigned)vl,
                           (unsigned)(sizeof(info->tags[i].value) - 1));
        }
        pos += kl + vl;
    }
    if (out_actual >= pos + 8u)
        memcpy(&info->deleted_until, out + out_actual - 8u, 8);
    return 0;
}

/* =========================================================================
 *  READ / WRITE — single syscall, no chunking
 * ========================================================================= */

/* fread / fwrite return the byte count transferred (0 .. 4 GiB) or a negative
 * -error_t on failure. The STORAGE wire field is 32-bit, so a single call moves
 * at most UINT32_MAX bytes; a larger request is capped (a short transfer — the
 * caller loops for more) rather than silently wrapped, and a write refused
 * part way is a short transfer too. The int64 return keeps a
 * 2..4 GiB count on the non-negative side, clear of the -error_t cause channel
 * (every error_t <= 1100); the C++ box::result bridge is box::_detail::from_ret64. */
int64_t fread(file_id_t file_id, uint64_t offset, void *buffer, size_t size)
{
    if (!buffer || size == 0) return -ERR_INVALID_ARGUMENT;
    uint32_t req = size > UINT32_MAX ? UINT32_MAX : (uint32_t)size;

    /* params: [u64 file_id][u64 offset]. */
    uint8_t params[16];
    memcpy(params,     &file_id, 8);
    memcpy(params + 8, &offset,  8);

    uint32_t out_actual = 0;
    int rc = MfCall1(DECK_STORAGE, STORAGE_OBJ_READ,
                     params, sizeof(params),
                     NULL, 0,
                     buffer, req, &out_actual,
                     BOX_ANSWER_GUARANTEED, NULL);
    if (rc != 0) return box_fail(rc);
    return (int64_t)out_actual;            /* 0..req, always >= 0 */
}

/* obj.write with `flags` (STORAGE_WRITE_*): fwrite's answer. */
static int64_t write_with(file_id_t file_id, uint64_t offset, uint32_t flags,
                          const void *buffer, size_t size)
{
    if (!buffer || size == 0) return -ERR_INVALID_ARGUMENT;
    uint32_t req = size > UINT32_MAX ? UINT32_MAX : (uint32_t)size;

    /* params: [u64 file_id][u64 offset][u32 flags]. */
    uint8_t params[20];
    memcpy(params,      &file_id, 8);
    memcpy(params + 8,  &offset,  8);
    memcpy(params + 16, &flags,   4);

    /* The stats crate [u64 written][u64 size] comes back on every answer, a
     * refusal included: the bytes the file holds of this write. A refusal
     * part way through is a short count — the file holds that much, and the
     * next call, from there, meets the refusal itself — never an error that
     * would have the caller write again what already landed. */
    uint8_t  out[16] = {0};
    uint32_t out_actual = 0;
    int rc = MfCall1(DECK_STORAGE, STORAGE_OBJ_WRITE,
                     params, sizeof(params),
                     buffer, req,
                     out, sizeof(out), &out_actual,
                     BOX_ANSWER_GUARANTEED, NULL);
    uint64_t bytes_written = 0;
    if (out_actual >= 8)
        memcpy(&bytes_written, out, 8);
    if (rc != 0)
        return bytes_written > 0 ? (int64_t)bytes_written : box_fail(rc);
    if (out_actual < 8) return (int64_t)req; /* op succeeded; assume full write of the submitted crate */
    return (int64_t)bytes_written;
}

int64_t fwrite(file_id_t file_id, uint64_t offset, const void *buffer, size_t size)
{
    return write_with(file_id, offset, 0, buffer, size);
}

int64_t file_append(file_id_t file_id, const void *buffer, size_t size)
{
    return write_with(file_id, 0, STORAGE_WRITE_APPEND, buffer, size);
}

/* =========================================================================
 *  RENAME / DELETE
 * ========================================================================= */

int file_rename(file_id_t file_id, const char *new_filename)
{
    if (!new_filename) return -ERR_INVALID_ARGUMENT;
    size_t fn_len = strlen(new_filename);
    if (fn_len == 0 || fn_len >= 64) return -ERR_INVALID_ARGUMENT;

    /* params: [u64 file_id][u16 name_len][char name[name_len]]. */
    uint8_t params[10 + 64];
    uint16_t name_len = (uint16_t)fn_len;
    memcpy(params,     &file_id,  8);
    memcpy(params + 8, &name_len, 2);
    memcpy(params + 10, new_filename, fn_len);

    int rc = MfCall1(DECK_STORAGE, STORAGE_OBJ_RENAME,
                     params, (uint16_t)(10 + fn_len),
                     NULL, 0, NULL, 0, NULL,
                     BOX_ANSWER_GUARANTEED, NULL);
    return box_fail(rc);
}

int delete(file_id_t file_id)
{
    int rc = MfCall1(DECK_STORAGE, STORAGE_OBJ_DELETE,
                     &file_id, sizeof(file_id),
                     NULL, 0, NULL, 0, NULL,
                     BOX_ANSWER_GUARANTEED, NULL);
    return box_fail(rc);
}

int file_span(file_id_t file_id, uint64_t size, uint32_t flags)
{
    /* params: [u64 file_id][u64 size][u32 flags]. */
    uint8_t params[20];
    memcpy(params,      &file_id, 8);
    memcpy(params + 8,  &size,    8);
    memcpy(params + 16, &flags,   4);

    int rc = MfCall1(DECK_STORAGE, STORAGE_OBJ_SPAN,
                     params, sizeof(params),
                     NULL, 0, NULL, 0, NULL,
                     BOX_ANSWER_GUARANTEED, NULL);
    return box_fail(rc);
}

/* =========================================================================
 *  TAGS
 * ========================================================================= */

int tag_add(file_id_t file_id, const char *tag)
{
    if (!tag || tag[0] == '\0') return -ERR_INVALID_ARGUMENT;
    size_t tag_len = strlen(tag);
    int rc = MfCall1(DECK_STORAGE, STORAGE_TAG_SET,
                     &file_id, sizeof(file_id),
                     tag, (uint32_t)tag_len,
                     NULL, 0, NULL,
                     BOX_ANSWER_GUARANTEED, NULL);
    return box_fail(rc);
}

int tag_remove(file_id_t file_id, const char *key)
{
    if (!key || key[0] == '\0') return -ERR_INVALID_ARGUMENT;
    size_t kl = strlen(key);
    int rc = MfCall1(DECK_STORAGE, STORAGE_TAG_UNSET,
                     &file_id, sizeof(file_id),
                     key, (uint32_t)kl,
                     NULL, 0, NULL,
                     BOX_ANSWER_GUARANTEED, NULL);
    return box_fail(rc);
}

int file_keep(file_id_t file_id, uint64_t term_seconds)
{
    uint64_t params[2] = { file_id, term_seconds };
    int rc = MfCall1(DECK_STORAGE, STORAGE_OBJ_KEEP,
                     params, sizeof(params),
                     NULL, 0, NULL, 0, NULL,
                     BOX_ANSWER_GUARANTEED, NULL);
    return box_fail(rc);
}

int file_restore(file_id_t file_id)
{
    int rc = MfCall1(DECK_STORAGE, STORAGE_OBJ_RESTORE,
                     &file_id, sizeof(file_id),
                     NULL, 0, NULL, 0, NULL,
                     BOX_ANSWER_GUARANTEED, NULL);
    return box_fail(rc);
}

int find_file_by_name(const char *filename, const char *without, file_id_t *file_ids,
                      file_info_t *out_infos, size_t max)
{
    if (!filename || filename[0] == '\0') return -ERR_INVALID_ARGUMENT;
    if (max > 0 && !file_ids) return -ERR_INVALID_ARGUMENT;

    /* The name index answers for every file whose name or stem is the word, so
     * the whole name asked for finds its files in one ask, however many files
     * there are; the names are held to the whole of it here. What stood here
     * once walked the first 255 files of the volume and asked each its name. */
    file_id_t *ids = NULL;
    int n = query_name_all(filename, NULL, without, &ids);
    if (n < 0) return n;

    int found = 0;
    for (int i = 0; i < n; i++) {
        file_info_t info;
        if (file_info(ids[i], &info) != 0) continue;
        if (strcmp(info.filename, filename) != 0) continue;
        if ((size_t)found < max) {
            file_ids[found] = ids[i];
            if (out_infos) out_infos[found] = info;
        }
        found++;
    }
    free(ids);
    return found;
}


/* =========================================================================
 *  Durability — anchor()
 * ========================================================================= */

int anchor(file_id_t file_id)
{
    int rc = MfCall1(DECK_STORAGE, STORAGE_OBJ_ANCHOR,
                     &file_id, sizeof(file_id),
                     NULL, 0, NULL, 0, NULL,
                     BOX_ANSWER_GUARANTEED, NULL);
    return box_fail(rc);
}

int write_settled(file_id_t file_id)
{
    int rc = MfCall1(DECK_STORAGE, STORAGE_OBJ_WRITE_SETTLED,
                     &file_id, sizeof(file_id),
                     NULL, 0, NULL, 0, NULL,
                     BOX_ANSWER_GUARANTEED, NULL);
    return box_fail(rc);
}

int escheat(EscheatAnswer* said)
{
    int rc = MfCall1(DECK_STORAGE, STORAGE_OBJ_ESCHEAT, NULL, 0, NULL, 0, said, sizeof(*said), NULL,
                     BOX_ANSWER_GUARANTEED, NULL);
    return box_fail(rc);
}

int conjugate(file_id_t fid, uint64_t at, file_id_t onto, uint64_t onto_at, uint64_t count, ConjugateAnswer* said)
{
    uint64_t params[5] = { (uint64_t)fid, at, (uint64_t)onto, onto_at, count };
    int      rc        = MfCall1(DECK_STORAGE, STORAGE_OBJ_CONJUGATE, params, sizeof(params), NULL, 0, said,
                                 sizeof(*said), NULL, BOX_ANSWER_GUARANTEED, NULL);
    return box_fail(rc);
}
