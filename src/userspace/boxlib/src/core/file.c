#include "box/chain.h"
#include "box/file.h"
#include "box/notify.h"
#include "box/result.h"
#include "box/string.h"
#include "box/types.h"

int create(const char *filename, const char *tags)
{
    if (!filename || filename[0] == '\0') return -1;
    size_t fn_len = strlen(filename);
    if (fn_len >= 32) return -1;

    obj_create(filename, tags);
    event_id_t event_id = notify();
    if (event_id == 0) return -1;

    result_entry_t result;
    if (!result_wait(&result, 5000)) return -1;
    if (result.error_code != OK) return -1;
    if (result.size < 8) return -1;

    uint32_t file_id, error_code;
    memcpy(&file_id, result.payload, 4);
    memcpy(&error_code, result.payload + 4, 4);
    if (error_code != 0) return -1;
    return (int)file_id;
}

int query(const char *tags, uint32_t *file_ids, size_t max_files)
{
    if (!file_ids || max_files == 0) return -1;

    obj_query(tags);
    event_id_t event_id = notify();
    if (event_id == 0) return -1;

    result_entry_t result;
    if (!result_wait(&result, 5000)) return -1;
    if (result.error_code != OK) return -1;
    if (result.size < 4) return -1;

    uint32_t count;
    memcpy(&count, result.payload, 4);
    if (count > max_files) count = max_files;
    for (uint32_t i = 0; i < count; i++) {
        size_t offset = 4 + (i * 4);
        if (offset + 4 <= result.size) {
            memcpy(&file_ids[i], result.payload + offset, 4);
        }
    }
    return (int)count;
}

int file_info(uint32_t file_id, file_info_t *info)
{
    if (!info) return -1;

    obj_info(file_id);
    event_id_t event_id = notify();
    if (event_id == 0) return -1;

    result_entry_t result;
    if (!result_wait(&result, 5000)) return -1;
    if (result.error_code != OK) return -1;
    if (result.size < 192) return -1;

    int32_t error_code;
    memcpy(&error_code, result.payload, 4);
    if (error_code != 0) return -1;

    memcpy(&info->file_id,   result.payload + 4,  4);
    memcpy(&info->flags,     result.payload + 8,  4);
    memcpy(&info->size,      result.payload + 12, 8);
    memcpy(&info->tag_count, result.payload + 20, 1);
    memcpy(info->filename,   result.payload + 24, 32);

    uint8_t tag_count = (info->tag_count > 5) ? 5 : info->tag_count;
    for (uint8_t i = 0; i < tag_count; i++) {
        size_t tag_offset = 56 + (i * 24);
        memcpy(&info->tags[i].type,  result.payload + tag_offset,      1);
        memcpy(info->tags[i].key,    result.payload + tag_offset + 1,  11);
        memcpy(info->tags[i].value,  result.payload + tag_offset + 12, 12);
    }
    return 0;
}

int fread(uint32_t file_id, uint64_t offset, void *buffer, size_t size)
{
    if (!buffer || size == 0) return -1;
    if (size > 176) size = 176;

    obj_read(file_id, offset, (uint32_t)size);
    event_id_t event_id = notify();
    if (event_id == 0) return -1;

    result_entry_t result;
    if (!result_wait(&result, 5000)) return -1;
    if (result.error_code != OK) return -1;
    if (result.size < 16) return -1;

    uint64_t bytes_read;
    uint32_t error_code;
    memcpy(&bytes_read, result.payload,     8);
    memcpy(&error_code, result.payload + 8, 4);
    if (error_code != 0) return -1;

    if (bytes_read > 0) {
        if (bytes_read > size) bytes_read = size;
        if (result.size < 16 + bytes_read) return -1;
        memcpy(buffer, result.payload + 16, bytes_read);
    }
    return (int)bytes_read;
}

int fwrite(uint32_t file_id, uint64_t offset, const void *buffer, size_t size)
{
    if (!buffer || size == 0) return -1;
    if (size > 168) size = 168;

    obj_write(file_id, offset, buffer, (uint32_t)size);
    event_id_t event_id = notify();
    if (event_id == 0) return -1;

    result_entry_t result;
    if (!result_wait(&result, 5000)) return -1;
    if (result.error_code != OK) return -1;
    if (result.size < 20) return -1;

    uint64_t bytes_written;
    uint32_t error_code;
    memcpy(&bytes_written, result.payload,      8);
    memcpy(&error_code,    result.payload + 16, 4);
    if (error_code != 0) return -1;
    return (int)bytes_written;
}

int file_rename(uint32_t file_id, const char *new_filename)
{
    if (!new_filename) return -1;
    size_t fn_len = strlen(new_filename);
    if (fn_len == 0 || fn_len >= 32) return -1;

    obj_rename(file_id, new_filename);
    event_id_t event_id = notify();
    if (event_id == 0) return -1;

    result_entry_t result;
    if (!result_wait(&result, 5000)) return -1;
    if (result.error_code != OK) return -1;
    if (result.size < 4) return -1;

    int32_t error_code;
    memcpy(&error_code, result.payload, 4);
    return (error_code == 0) ? 0 : -1;
}

int delete(uint32_t file_id)
{
    obj_delete(file_id);
    event_id_t event_id = notify();
    if (event_id == 0) return -1;

    result_entry_t result;
    if (!result_wait(&result, 5000)) return -1;
    if (result.error_code != OK) return -1;
    if (result.size < 4) return -1;

    int32_t error_code;
    memcpy(&error_code, result.payload, 4);
    return (error_code == 0) ? 0 : -1;
}

int tag_add(uint32_t file_id, const char *tag)
{
    if (!tag || tag[0] == '\0') return -1;
    size_t tag_len = strlen(tag);
    if (tag_len >= 32) return -1;

    obj_tag_set(file_id, tag);
    event_id_t event_id = notify();
    if (event_id == 0) return -1;

    result_entry_t result;
    if (!result_wait(&result, 5000)) return -1;
    if (result.error_code != OK) return -1;
    return 0;
}

int tag_remove(uint32_t file_id, const char *key)
{
    if (!key || key[0] == '\0') return -1;
    size_t key_len = strlen(key);
    if (key_len >= 32) return -1;

    obj_tag_unset(file_id, key);
    event_id_t event_id = notify();
    if (event_id == 0) return -1;

    result_entry_t result;
    if (!result_wait(&result, 5000)) return -1;
    if (result.error_code != OK) return -1;
    return 0;
}

int context_set(const char *tag)
{
    if (!tag || tag[0] == '\0') return -1;
    size_t tag_len = strlen(tag);
    if (tag_len >= 32) return -1;

    ctx_set(tag);
    event_id_t event_id = notify();
    if (event_id == 0) return -1;

    result_entry_t result;
    if (!result_wait(&result, 5000)) return -1;
    if (result.error_code != OK) return -1;
    return 0;
}

int context_clear(void)
{
    ctx_clear();
    event_id_t event_id = notify();
    if (event_id == 0) return -1;

    result_entry_t result;
    if (!result_wait(&result, 5000)) return -1;
    if (result.error_code != OK) return -1;
    return 0;
}

int fwrite_all(uint32_t file_id, const void *buffer, size_t total_size)
{
    const uint8_t *src = (const uint8_t *)buffer;
    size_t written = 0;
    const size_t chunk_size = 168;

    while (written < total_size) {
        size_t to_write = total_size - written;
        if (to_write > chunk_size) to_write = chunk_size;
        int result = fwrite(file_id, written, src + written, to_write);
        if (result < 0) return -1;
        written += result;
        if ((size_t)result < to_write) break;
    }
    return (int)written;
}

int fread_all(uint32_t file_id, void *buffer, size_t max_size, size_t *bytes_read)
{
    uint8_t *dest = (uint8_t *)buffer;
    size_t total_read = 0;
    const size_t chunk_size = 168;

    while (total_read < max_size) {
        size_t to_read = max_size - total_read;
        if (to_read > chunk_size) to_read = chunk_size;
        int result = fread(file_id, total_read, dest + total_read, to_read);
        if (result < 0) return -1;
        total_read += result;
        if (result == 0 || (size_t)result < to_read) break;
    }
    if (bytes_read) *bytes_read = total_read;
    return 0;
}
