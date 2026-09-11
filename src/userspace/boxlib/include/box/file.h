#ifndef BOX_STORAGE_H
#define BOX_STORAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/defs.h"

#define FILE_FLAG_TRASHED  (1 << 1)

typedef struct {
    uint8_t type;
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
int create_everywhere(const char* filename, const char* tags);
int query(const char* tags, uint32_t* file_ids, size_t max_files);
int query_everywhere(const char* tags, uint32_t* file_ids, size_t max_files);
int query_all(const char* tags, uint32_t** out_ids);
int query_all_everywhere(const char* tags, uint32_t** out_ids);
int file_info(uint32_t file_id, file_info_t* info);
int64_t fread(uint32_t file_id, uint64_t offset, void* buffer, size_t size);
int64_t fwrite(uint32_t file_id, uint64_t offset, const void* buffer, size_t size);
#ifdef __cplusplus
int file_delete(uint32_t file_id) asm("delete");
#else
int delete(uint32_t file_id);
#endif
int file_rename(uint32_t file_id, const char* new_filename);

int file_truncate(uint32_t file_id, uint64_t new_size);

int tag_add(uint32_t file_id, const char* tag);
int tag_remove(uint32_t file_id, const char* key);

int find_file_by_name(const char* filename, uint32_t* file_ids, file_info_t* out_infos, size_t max);

int snap_create(const char *name, uint32_t file_id, uint32_t *out_snap_id);
int snap_delete(uint32_t snap_id);
int snap_list(uint32_t *out_ids, uint32_t max_ids, uint32_t *out_count);

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

int anchor(uint32_t file_id);

#ifdef __cplusplus
}
#endif

#endif