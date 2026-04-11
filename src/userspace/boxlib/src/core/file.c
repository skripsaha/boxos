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

    struct PACKED {
        char filename[32];
        char tags[160];
    } args;
    memset(&args, 0, sizeof(args));
    memcpy(args.filename, filename, fn_len);
    if (tags && tags[0] != '\0') {
        size_t tag_len = strlen(tags);
        if (tag_len < 160) memcpy(args.tags, tags, tag_len);
    }
    pocket_send(DECK_STORAGE, 0x07, &args, sizeof(args));

    Result result;
    if (!result_wait(&result, 5000)) return -1;
    if (result.error_code != OK) return -1;
    if (result.data_length < 8) return -1;
    if (!result.data_addr) return -1;

    uint8_t *data = (uint8_t *)(uintptr_t)result.data_addr;
    uint32_t file_id, error_code;
    memcpy(&file_id,    data,     4);
    memcpy(&error_code, data + 4, 4);
    if (error_code != 0) return -1;
    return (int)file_id;
}

int query(const char *tags, uint32_t *file_ids, size_t max_files)
{
    if (!file_ids || max_files == 0) return -1;

    uint8_t args[192];
    memset(args, 0, sizeof(args));
    if (tags && tags[0] != '\0') {
        size_t len = strlen(tags);
        if (len < 192) memcpy(args, tags, len);
    }
    pocket_send(DECK_STORAGE, 0x01, args, sizeof(args));

    Result result;
    if (!result_wait(&result, 5000)) return -1;
    if (result.error_code != OK) return -1;
    if (result.data_length < 4) return -1;
    if (!result.data_addr) return -1;

    uint8_t *data = (uint8_t *)(uintptr_t)result.data_addr;
    uint32_t count;
    memcpy(&count, data, 4);
    if (count > max_files) count = max_files;
    for (uint32_t i = 0; i < count; i++) {
        size_t offset = 4 + (i * 4);
        if (offset + 4 <= result.data_length) {
            memcpy(&file_ids[i], data + offset, 4);
        }
    }
    return (int)count;
}

int file_info(uint32_t file_id, file_info_t *info)
{
    if (!info) return -1;

    struct PACKED {
        uint32_t file_id;
        uint8_t reserved[188];
    } args;
    memset(&args, 0, sizeof(args));
    args.file_id = file_id;
    pocket_send(DECK_STORAGE, 0x0A, &args, sizeof(args));

    Result result;
    if (!result_wait(&result, 5000)) return -1;
    if (result.error_code != OK) return -1;
    if (result.data_length < 192) return -1;
    if (!result.data_addr) return -1;

    uint8_t *data = (uint8_t *)(uintptr_t)result.data_addr;
    int32_t error_code;
    memcpy(&error_code, data, 4);
    if (error_code != 0) return -1;

    memcpy(&info->file_id,   data + 4,  4);
    memcpy(&info->flags,     data + 8,  4);
    memcpy(&info->size,      data + 12, 8);
    memcpy(&info->tag_count, data + 20, 1);
    memcpy(info->filename,   data + 24, 32);

    uint8_t tag_count = (info->tag_count > 5) ? 5 : info->tag_count;
    for (uint8_t i = 0; i < tag_count; i++) {
        size_t tag_offset = 56 + (i * 24);
        memcpy(&info->tags[i].type,  data + tag_offset,      1);
        memcpy(info->tags[i].key,    data + tag_offset + 1,  11);
        memcpy(info->tags[i].value,  data + tag_offset + 12, 12);
    }
    return 0;
}

int fread(uint32_t file_id, uint64_t offset, void *buffer, size_t size)
{
    if (!buffer || size == 0) return -1;
    if (size > 176) size = 176;

    struct PACKED {
        uint32_t file_id;
        uint64_t offset;
        uint32_t length;
        uint8_t reserved[176];
    } args;
    memset(&args, 0, sizeof(args));
    args.file_id = file_id;
    args.offset  = offset;
    args.length  = (uint32_t)size;
    pocket_send(DECK_STORAGE, 0x05, &args, sizeof(args));

    Result result;
    if (!result_wait(&result, 5000)) return -1;
    if (result.error_code != OK) return -1;
    if (result.data_length < 16) return -1;
    if (!result.data_addr) return -1;

    uint8_t *data = (uint8_t *)(uintptr_t)result.data_addr;
    uint64_t bytes_read;
    uint32_t error_code;
    memcpy(&bytes_read, data,     8);
    memcpy(&error_code, data + 8, 4);
    if (error_code != 0) return -1;

    if (bytes_read > 0) {
        if (bytes_read > size) bytes_read = size;
        if (result.data_length < 16 + bytes_read) return -1;
        memcpy(buffer, data + 16, bytes_read);
    }
    return (int)bytes_read;
}

int fwrite(uint32_t file_id, uint64_t offset, const void *buffer, size_t size)
{
    if (!buffer || size == 0) return -1;
    if (size > 168) size = 168;

    struct PACKED {
        uint32_t file_id;
        uint64_t offset;
        uint32_t length;
        uint32_t flags;
        uint8_t data[168];
        uint8_t reserved[4];
    } args;
    memset(&args, 0, sizeof(args));
    args.file_id = file_id;
    args.offset  = offset;
    args.length  = (uint32_t)size;
    args.flags   = 0;
    memcpy(args.data, buffer, size);
    pocket_send(DECK_STORAGE, 0x06, &args, sizeof(args));

    Result result;
    if (!result_wait(&result, 5000)) return -1;
    if (result.error_code != OK) return -1;
    if (result.data_length < 20) return -1;
    if (!result.data_addr) return -1;

    uint8_t *data = (uint8_t *)(uintptr_t)result.data_addr;
    uint64_t bytes_written;
    uint32_t error_code;
    memcpy(&bytes_written, data,      8);
    memcpy(&error_code,    data + 16, 4);
    if (error_code != 0) return -1;
    return (int)bytes_written;
}

int file_rename(uint32_t file_id, const char *new_filename)
{
    if (!new_filename) return -1;
    size_t fn_len = strlen(new_filename);
    if (fn_len == 0 || fn_len >= 32) return -1;

    struct PACKED {
        uint32_t file_id;
        char new_filename[32];
        uint8_t reserved[156];
    } args;
    memset(&args, 0, sizeof(args));
    args.file_id = file_id;
    memcpy(args.new_filename, new_filename, fn_len);
    pocket_send(DECK_STORAGE, 0x09, &args, sizeof(args));

    Result result;
    if (!result_wait(&result, 5000)) return -1;
    if (result.error_code != OK) return -1;
    if (result.data_length < 4) return -1;
    if (!result.data_addr) return -1;

    uint8_t *data = (uint8_t *)(uintptr_t)result.data_addr;
    int32_t error_code;
    memcpy(&error_code, data, 4);
    return (error_code == 0) ? 0 : -1;
}

int delete(uint32_t file_id)
{
    struct PACKED {
        uint32_t file_id;
        uint8_t reserved[188];
    } args;
    memset(&args, 0, sizeof(args));
    args.file_id = file_id;
    pocket_send(DECK_STORAGE, 0x08, &args, sizeof(args));

    Result result;
    if (!result_wait(&result, 5000)) return -1;
    if (result.error_code != OK) return -1;
    if (result.data_length < 4) return -1;
    if (!result.data_addr) return -1;

    uint8_t *data = (uint8_t *)(uintptr_t)result.data_addr;
    int32_t error_code;
    memcpy(&error_code, data, 4);
    return (error_code == 0) ? 0 : -1;
}

int tag_add(uint32_t file_id, const char *tag)
{
    if (!tag || tag[0] == '\0') return -1;
    size_t tag_len = strlen(tag);
    if (tag_len >= 32) return -1;

    struct PACKED {
        uint32_t file_id;
        char tag[32];
        uint8_t reserved[156];
    } args;
    memset(&args, 0, sizeof(args));
    args.file_id = file_id;
    memcpy(args.tag, tag, tag_len);
    pocket_send(DECK_STORAGE, 0x02, &args, sizeof(args));

    Result result;
    if (!result_wait(&result, 5000)) return -1;
    if (result.error_code != OK) return -1;
    return 0;
}

int tag_remove(uint32_t file_id, const char *key)
{
    if (!key || key[0] == '\0') return -1;
    size_t key_len = strlen(key);
    if (key_len >= 32) return -1;

    struct PACKED {
        uint32_t file_id;
        char tag[32];
        uint8_t reserved[156];
    } args;
    memset(&args, 0, sizeof(args));
    args.file_id = file_id;
    memcpy(args.tag, key, key_len);
    pocket_send(DECK_STORAGE, 0x03, &args, sizeof(args));

    Result result;
    if (!result_wait(&result, 5000)) return -1;
    if (result.error_code != OK) return -1;
    return 0;
}

int context_set(const char *tag)
{
    if (!tag || tag[0] == '\0') return -1;
    size_t tag_len = strlen(tag);
    if (tag_len >= 32) return -1;

    uint8_t args[192];
    memset(args, 0, sizeof(args));
    memcpy(args, tag, tag_len);
    pocket_send(DECK_STORAGE, 0x10, args, sizeof(args));

    Result result;
    if (!result_wait(&result, 5000)) return -1;
    if (result.error_code != OK) return -1;
    return 0;
}

int context_clear(void)
{
    uint8_t args[192];
    memset(args, 0, sizeof(args));
    pocket_send(DECK_STORAGE, 0x11, args, sizeof(args));

    Result result;
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

int find_file_by_name(const char *filename, uint32_t *file_ids,
                      file_info_t *out_infos, size_t max)
{
    uint32_t all_files[256];
    int total = query(NULL, all_files, 256);
    if (total < 0) return -1;
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
