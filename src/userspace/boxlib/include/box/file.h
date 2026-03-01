#ifndef BOX_FILE_H
#define BOX_FILE_H

#include "box/defs.h"

#define FILE_FLAG_ACTIVE   (1 << 0)
#define FILE_FLAG_TRASHED  (1 << 1)
#define FILE_FLAG_HIDDEN   (1 << 2)

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
int fread(uint32_t file_id, uint64_t offset, void* buffer, size_t size);
int fwrite(uint32_t file_id, uint64_t offset, const void* buffer, size_t size);
int delete(uint32_t file_id);
int file_rename(uint32_t file_id, const char* new_filename);

int tag_add(uint32_t file_id, const char* tag);
int tag_remove(uint32_t file_id, const char* key);

int context_set(const char* tag);
int context_clear(void);

int fwrite_all(uint32_t file_id, const void* buffer, size_t total_size);
int fread_all(uint32_t file_id, void* buffer, size_t max_size, size_t* bytes_read);

int find_file_by_name(const char* filename, uint32_t* file_ids, file_info_t* out_infos, size_t max);

#endif // BOX_FILE_H
