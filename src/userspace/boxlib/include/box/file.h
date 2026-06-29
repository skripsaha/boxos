#ifndef BOX_STORAGE_H
#define BOX_STORAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/defs.h"

#define FILE_FLAG_TRASHED  (1 << 1)

typedef struct {
    uint8_t type;        // 0=user, 1=system
    char key[11];
    char value[12];
} tag_t;

typedef struct {
    uint32_t file_id;
    uint32_t flags;
    uint64_t size;
    uint8_t tag_count;
    char filename[32];
    tag_t tags[5];
} file_info_t;

int create(const char* filename, const char* tags);
int query(const char* tags, uint32_t* file_ids, size_t max_files);
int file_info(uint32_t file_id, file_info_t* info);
/* Return the byte count transferred (0..4 GiB, short-transfer: a request > 4 GiB
 * is capped to one syscall — loop for more), or a negative -error_t on failure. */
int64_t fread(uint32_t file_id, uint64_t offset, void* buffer, size_t size);
int64_t fwrite(uint32_t file_id, uint64_t offset, const void* buffer, size_t size);
/* `delete` is a C++ keyword — C++ callers use file_delete(), bound to the
 * same ELF symbol via asm label. C keeps the original name unchanged. */
#ifdef __cplusplus
int file_delete(uint32_t file_id) asm("delete");
#else
int delete(uint32_t file_id);
#endif
int file_rename(uint32_t file_id, const char* new_filename);

int tag_add(uint32_t file_id, const char* tag);
int tag_remove(uint32_t file_id, const char* key);

int context_set(const char* tag);
int context_clear(void);
/* context_get — report the calling process's current context tags
 * ("key" or "key:value", each up to 63 chars + NUL) into the caller's
 * fixed-width table. *out_count receives the number written (capped at
 * max_tags). Use it to save the context before installing your own and
 * restore it on exit. Returns 0 on success, -1 on error. */
int context_get(char out_tags[][64], uint32_t max_tags, uint32_t* out_count);

int find_file_by_name(const char* filename, uint32_t* file_ids, file_info_t* out_infos, size_t max);

/* CoW snapshots — capture a frozen view of a file (or all files when
 * file_id == 0). The snapshot's redirected blocks are released back to
 * the allocator at snap_delete. */
int snap_create(const char *name, uint32_t file_id, uint32_t *out_snap_id);
int snap_delete(uint32_t snap_id);
int snap_list(uint32_t *out_ids, uint32_t max_ids, uint32_t *out_count);

/* Structured per-snapshot record by id — the name-by-id lookup snap_list (ids
 * only) lacks, the basis for resolving a snapshot by name. */
typedef struct {
    uint32_t id;
    uint32_t parent_file_id;
    uint64_t created_time;
    uint32_t file_count;
    uint64_t total_size;
    uint8_t  flags;
    char     name[32];
} snap_info_t;

int snap_info(uint32_t snap_id, snap_info_t *out);

/*
 * anchor() — durability primitive. Forces all in-memory metadata to
 * disk and flushes the disk cache. Returns when persisted.
 *
 * Two ways to use it:
 *   anchor(fid)        — explicit blocking call. POSIX-shaped.
 *   touch_claim("anchor", REST, 0, 0); ... fwrite(...); anchor(fid);
 *                       — observers subscribed on "anchor" wake up
 *                         with payload {file_id, op=2, ...}, plus on
 *                         every tag of the anchored file. So a
 *                         monitor app can confirm durability without
 *                         polling.
 *
 * file_id == 0 anchors the whole filesystem and publishes only the
 * generic "anchor" event (no per-tag fan-out).
 */
int anchor(uint32_t file_id);

#ifdef __cplusplus
}
#endif

#endif // BOX_STORAGE_H
