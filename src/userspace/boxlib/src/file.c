/*
 * file.c — userspace file/tag/context wrappers (Phase 12: Manifest-only).
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

#define STORAGE_TAG_QUERY       0x01
#define STORAGE_TAG_SET         0x02
#define STORAGE_TAG_UNSET       0x03
#define STORAGE_OBJ_READ        0x05
#define STORAGE_OBJ_WRITE       0x06
#define STORAGE_OBJ_CREATE      0x07
#define STORAGE_OBJ_DELETE      0x08
#define STORAGE_OBJ_RENAME      0x09
#define STORAGE_OBJ_GET_INFO    0x0A
#define STORAGE_CONTEXT_SET     0x10
#define STORAGE_CONTEXT_CLEAR   0x11
#define STORAGE_CONTEXT_GET     0x12
#define STORAGE_SNAP_CREATE     0x20
#define STORAGE_SNAP_DELETE     0x21
#define STORAGE_SNAP_LIST       0x22
#define STORAGE_OBJ_ANCHOR      0x23

#define STORAGE_TIMEOUT_MS      5000u

/* =========================================================================
 *  CREATE / QUERY
 * ========================================================================= */

int create(const char *filename, const char *tags)
{
    if (!filename || filename[0] == '\0') return -ERR_INVALID_ARGUMENT;
    size_t fn_len = strlen(filename);
    if (fn_len >= 32) return -ERR_INVALID_ARGUMENT;

    /* params: 32 bytes, NUL-padded filename. */
    uint8_t params[32] = {0};
    memcpy(params, filename, fn_len);

    /* in_crate: tag list (optional). */
    const void *in = NULL;
    uint32_t    in_size = 0;
    if (tags && tags[0] != '\0') {
        in = tags;
        in_size = (uint32_t)strlen(tags);
    }

    uint32_t file_id = 0;
    int rc = MfCall1(DECK_STORAGE, STORAGE_OBJ_CREATE,
                     params, sizeof(params),
                     in, in_size,
                     &file_id, sizeof(file_id), NULL,
                     STORAGE_TIMEOUT_MS, NULL);
    if (rc != 0) return box_fail(rc);
    return (int)file_id;
}

int query(const char *tags, uint32_t *file_ids, size_t max_files)
{
    if (!file_ids || max_files == 0) return -ERR_INVALID_ARGUMENT;

    /* out_crate: [u32 count][u32 ids[max_files]]. */
    uint32_t out_cap = (uint32_t)(4 + max_files * sizeof(uint32_t));
    uint8_t  stack_buf[1024];
    uint8_t *out = stack_buf;
    if (out_cap > sizeof(stack_buf)) out_cap = (uint32_t)sizeof(stack_buf);

    const void *in = NULL;
    uint32_t    in_size = 0;
    if (tags && tags[0] != '\0') {
        in = tags;
        in_size = (uint32_t)strlen(tags);
    }

    uint32_t out_actual = 0;
    int rc = MfCall1(DECK_STORAGE, STORAGE_TAG_QUERY,
                     NULL, 0,
                     in, in_size,
                     out, out_cap, &out_actual,
                     STORAGE_TIMEOUT_MS, NULL);
    if (rc != 0) return box_fail(rc);
    if (out_actual < 4) return 0;

    uint32_t count;
    memcpy(&count, out, 4);
    if (count > max_files) count = (uint32_t)max_files;
    for (uint32_t i = 0; i < count; i++) {
        memcpy(&file_ids[i], out + 4 + i * 4, 4);
    }
    return (int)count;
}

/* =========================================================================
 *  FILE_INFO — parse the variable-length blob ObjGetInfo writes.
 * ========================================================================= */

int file_info(uint32_t file_id, file_info_t *info)
{
    if (!info) return -ERR_INVALID_ARGUMENT;

    uint8_t  out[1024];
    uint32_t out_actual = 0;
    int rc = MfCall1(DECK_STORAGE, STORAGE_OBJ_GET_INFO,
                     &file_id, sizeof(file_id),
                     NULL, 0,
                     out, sizeof(out), &out_actual,
                     STORAGE_TIMEOUT_MS, NULL);
    if (rc != 0) return box_fail(rc);
    if (out_actual < 20) return -ERR_INTERNAL;

    memset(info, 0, sizeof(*info));

    size_t pos = 0;
    memcpy(&info->file_id, out + pos, 4); pos += 4;
    memcpy(&info->flags,   out + pos, 4); pos += 4;
    memcpy(&info->size,    out + pos, 8); pos += 8;
    uint16_t tag_count_raw = 0, fn_len = 0;
    memcpy(&tag_count_raw, out + pos, 2); pos += 2;
    memcpy(&fn_len,        out + pos, 2); pos += 2;

    if (pos + fn_len > out_actual) return -ERR_INTERNAL;
    size_t fn_copy = fn_len < sizeof(info->filename) - 1 ? fn_len
                                                          : sizeof(info->filename) - 1;
    memcpy(info->filename, out + pos, fn_copy);
    info->filename[fn_copy] = '\0';
    pos += fn_len;

    info->tag_count = (uint8_t)(tag_count_raw > 5 ? 5 : tag_count_raw);

    for (uint16_t i = 0; i < tag_count_raw; i++) {
        if (pos + 4 > out_actual) break;
        uint16_t kl = 0, vl = 0;
        memcpy(&kl, out + pos, 2); pos += 2;
        memcpy(&vl, out + pos, 2); pos += 2;
        if (pos + kl + vl > out_actual) break;

        if (i < 5) {
            size_t kc = kl < sizeof(info->tags[i].key) - 1 ? kl
                                                            : sizeof(info->tags[i].key) - 1;
            size_t vc = vl < sizeof(info->tags[i].value) - 1 ? vl
                                                              : sizeof(info->tags[i].value) - 1;
            memcpy(info->tags[i].key,   out + pos,       kc);
            info->tags[i].key[kc] = '\0';
            memcpy(info->tags[i].value, out + pos + kl,  vc);
            info->tags[i].value[vc] = '\0';
            info->tags[i].type = 0;
        }
        pos += kl + vl;
    }
    return 0;
}

/* =========================================================================
 *  READ / WRITE — single syscall, no chunking
 * ========================================================================= */

int fread(uint32_t file_id, uint64_t offset, void *buffer, size_t size)
{
    if (!buffer || size == 0) return -ERR_INVALID_ARGUMENT;

    /* params: [u32 file_id][u64 offset]. */
    uint8_t params[12];
    memcpy(params,     &file_id, 4);
    memcpy(params + 4, &offset,  8);

    uint32_t out_actual = 0;
    int rc = MfCall1(DECK_STORAGE, STORAGE_OBJ_READ,
                     params, sizeof(params),
                     NULL, 0,
                     buffer, (uint32_t)size, &out_actual,
                     STORAGE_TIMEOUT_MS, NULL);
    if (rc != 0) return box_fail(rc);
    return (int)out_actual;
}

int fwrite(uint32_t file_id, uint64_t offset, const void *buffer, size_t size)
{
    if (!buffer || size == 0) return -ERR_INVALID_ARGUMENT;

    /* params: [u32 file_id][u64 offset][u32 flags=0]. */
    uint8_t params[16];
    uint32_t flags = 0;
    memcpy(params,      &file_id, 4);
    memcpy(params + 4,  &offset,  8);
    memcpy(params + 12, &flags,   4);

    uint8_t  out[16] = {0};
    uint32_t out_actual = 0;
    int rc = MfCall1(DECK_STORAGE, STORAGE_OBJ_WRITE,
                     params, sizeof(params),
                     buffer, (uint32_t)size,
                     out, sizeof(out), &out_actual,
                     STORAGE_TIMEOUT_MS, NULL);
    if (rc != 0) return box_fail(rc);
    if (out_actual < 8) return (int)size; /* op succeeded; assume full write */

    uint64_t bytes_written = 0;
    memcpy(&bytes_written, out, 8);
    return (int)bytes_written;
}

/* =========================================================================
 *  RENAME / DELETE
 * ========================================================================= */

int file_rename(uint32_t file_id, const char *new_filename)
{
    if (!new_filename) return -ERR_INVALID_ARGUMENT;
    size_t fn_len = strlen(new_filename);
    if (fn_len == 0 || fn_len >= 64) return -ERR_INVALID_ARGUMENT;

    /* params: [u32 file_id][u16 name_len][char name[name_len]]. */
    uint8_t params[6 + 64];
    uint16_t name_len = (uint16_t)fn_len;
    memcpy(params,     &file_id,  4);
    memcpy(params + 4, &name_len, 2);
    memcpy(params + 6,  new_filename, fn_len);

    int rc = MfCall1(DECK_STORAGE, STORAGE_OBJ_RENAME,
                     params, (uint16_t)(6 + fn_len),
                     NULL, 0, NULL, 0, NULL,
                     STORAGE_TIMEOUT_MS, NULL);
    return box_fail(rc);
}

int delete(uint32_t file_id)
{
    int rc = MfCall1(DECK_STORAGE, STORAGE_OBJ_DELETE,
                     &file_id, sizeof(file_id),
                     NULL, 0, NULL, 0, NULL,
                     STORAGE_TIMEOUT_MS, NULL);
    return box_fail(rc);
}

/* =========================================================================
 *  TAGS / CONTEXT
 * ========================================================================= */

int tag_add(uint32_t file_id, const char *tag)
{
    if (!tag || tag[0] == '\0') return -ERR_INVALID_ARGUMENT;
    size_t tag_len = strlen(tag);
    if (tag_len >= 128) return -ERR_INVALID_ARGUMENT;
    int rc = MfCall1(DECK_STORAGE, STORAGE_TAG_SET,
                     &file_id, sizeof(file_id),
                     tag, (uint32_t)tag_len,
                     NULL, 0, NULL,
                     STORAGE_TIMEOUT_MS, NULL);
    return box_fail(rc);
}

int tag_remove(uint32_t file_id, const char *key)
{
    if (!key || key[0] == '\0') return -ERR_INVALID_ARGUMENT;
    size_t kl = strlen(key);
    if (kl >= 64) return -ERR_INVALID_ARGUMENT;
    int rc = MfCall1(DECK_STORAGE, STORAGE_TAG_UNSET,
                     &file_id, sizeof(file_id),
                     key, (uint32_t)kl,
                     NULL, 0, NULL,
                     STORAGE_TIMEOUT_MS, NULL);
    return box_fail(rc);
}

int context_set(const char *tag)
{
    if (!tag || tag[0] == '\0') return -ERR_INVALID_ARGUMENT;
    size_t tl = strlen(tag);
    if (tl >= 128) return -ERR_INVALID_ARGUMENT;
    int rc = MfCall1(DECK_STORAGE, STORAGE_CONTEXT_SET,
                     NULL, 0,
                     tag, (uint32_t)tl,
                     NULL, 0, NULL,
                     STORAGE_TIMEOUT_MS, NULL);
    return box_fail(rc);
}

int context_clear(void)
{
    int rc = MfCall1(DECK_STORAGE, STORAGE_CONTEXT_CLEAR,
                     NULL, 0, NULL, 0, NULL, 0, NULL,
                     STORAGE_TIMEOUT_MS, NULL);
    return box_fail(rc);
}

int context_get(char out_tags[][64], uint32_t max_tags, uint32_t *out_count)
{
    if (!out_tags || !out_count || max_tags == 0) return -ERR_INVALID_ARGUMENT;

    /* Sized to the kernel maximum: 4 + 64 tags * (2 len + 63 chars + 1). */
    uint8_t  buf[4 + 64 * 66];
    uint32_t out_actual = 0;
    int rc = MfCall1(DECK_STORAGE, STORAGE_CONTEXT_GET,
                     NULL, 0,
                     NULL, 0,
                     buf, sizeof(buf), &out_actual,
                     STORAGE_TIMEOUT_MS, NULL);
    if (rc != 0) return box_fail(rc);
    if (out_actual < 4) return -ERR_INTERNAL;

    uint32_t count = 0;
    memcpy(&count, buf, 4);

    uint32_t pos     = 4;
    uint32_t written = 0;
    for (uint32_t i = 0; i < count && written < max_tags; i++) {
        if (pos + 2 > out_actual) break;
        uint16_t len = 0;
        memcpy(&len, buf + pos, 2);
        pos += 2;
        if (pos + len > out_actual) break;
        size_t copy = len < 63 ? len : 63;
        memcpy(out_tags[written], buf + pos, copy);
        out_tags[written][copy] = '\0';
        pos += len;
        written++;
    }
    *out_count = written;
    return 0;
}

int find_file_by_name(const char *filename, uint32_t *file_ids,
                      file_info_t *out_infos, size_t max)
{
    uint32_t all_files[256];
    int total = query(NULL, all_files, 256);
    if (total < 0) return total;
    int match_count = 0;
    for (int i = 0; i < total && (size_t)match_count < max; i++) {
        file_info_t info;
        if (file_info(all_files[i], &info) == 0) {
            if (strcmp(info.filename, filename) == 0) {
                file_ids[match_count] = all_files[i];
                if (out_infos) out_infos[match_count] = info;
                match_count++;
            }
        }
    }
    return match_count;
}

/* =========================================================================
 *  CoW snapshot wrappers
 * ========================================================================= */

int snap_create(const char *name, uint32_t file_id, uint32_t *out_snap_id)
{
    if (!name || !out_snap_id) return -ERR_INVALID_ARGUMENT;
    size_t name_len = strlen(name);
    if (name_len == 0 || name_len > 31) return -ERR_INVALID_ARGUMENT;

    uint8_t params[5 + 32];
    memcpy(params,     &file_id, 4);
    uint8_t nl = (uint8_t)name_len;
    params[4] = nl;
    memcpy(params + 5, name, name_len);

    uint32_t out = 0;
    uint32_t out_actual = 0;
    int rc = MfCall1(DECK_STORAGE, STORAGE_SNAP_CREATE,
                     params, (uint16_t)(5 + name_len),
                     NULL, 0,
                     &out, sizeof(out), &out_actual,
                     STORAGE_TIMEOUT_MS, NULL);
    if (rc != 0) return box_fail(rc);
    if (out_actual < 4) return -ERR_INTERNAL;
    *out_snap_id = out;
    return 0;
}

int snap_delete(uint32_t snap_id)
{
    int rc = MfCall1(DECK_STORAGE, STORAGE_SNAP_DELETE,
                     &snap_id, sizeof(snap_id),
                     NULL, 0, NULL, 0, NULL,
                     STORAGE_TIMEOUT_MS, NULL);
    return box_fail(rc);
}

int snap_list(uint32_t *out_ids, uint32_t max_ids, uint32_t *out_count)
{
    if (!out_ids || !out_count || max_ids == 0) return -ERR_INVALID_ARGUMENT;
    uint32_t buf_bytes = 4 + max_ids * 4;
    uint8_t buf[4 + 64 * 4];
    if (max_ids > 64) max_ids = 64;
    buf_bytes = 4 + max_ids * 4;

    uint32_t out_actual = 0;
    int rc = MfCall1(DECK_STORAGE, STORAGE_SNAP_LIST,
                     NULL, 0,
                     NULL, 0,
                     buf, buf_bytes, &out_actual,
                     STORAGE_TIMEOUT_MS, NULL);
    if (rc != 0) return box_fail(rc);
    if (out_actual < 4) return -ERR_INTERNAL;
    uint32_t count;
    memcpy(&count, buf, 4);
    if (count > max_ids) count = max_ids;
    memcpy(out_ids, buf + 4, count * 4);
    *out_count = count;
    return 0;
}

/* =========================================================================
 *  Durability — anchor()
 * ========================================================================= */

int anchor(uint32_t file_id)
{
    int rc = MfCall1(DECK_STORAGE, STORAGE_OBJ_ANCHOR,
                     &file_id, sizeof(file_id),
                     NULL, 0, NULL, 0, NULL,
                     STORAGE_TIMEOUT_MS, NULL);
    return box_fail(rc);
}
