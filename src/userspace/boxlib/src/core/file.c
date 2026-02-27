#include "box/file.h"
#include "box/notify.h"
#include "box/result.h"
#include "box/string.h"
#include "box/types.h"

// Storage Deck opcodes
#define STORAGE_DECK_ID 0x02
#define STORAGE_TAG_QUERY 0x01
#define STORAGE_TAG_SET 0x02
#define STORAGE_TAG_UNSET 0x03
#define STORAGE_OBJ_READ 0x05
#define STORAGE_OBJ_WRITE 0x06
#define STORAGE_OBJ_CREATE 0x07
#define STORAGE_OBJ_DELETE 0x08
#define STORAGE_OBJ_RENAME 0x09
#define STORAGE_OBJ_GET_INFO 0x0A
#define STORAGE_CONTEXT_SET 0x10
#define STORAGE_CONTEXT_CLEAR 0x11

// ============================================================================
// FILE OPERATIONS
// ============================================================================

int create(const char *filename, const char *tags)
{
    if (!filename || filename[0] == '\0')
    {
        return -1;
    }

    size_t fn_len = strlen(filename);
    if (fn_len >= 32)
    {
        return -1;
    }

    notify_prepare();
    notify_add_prefix(STORAGE_DECK_ID, STORAGE_OBJ_CREATE);

    struct __attribute__((packed))
    {
        char filename[32];
        char tags[160];
    } request;

    memset(&request, 0, sizeof(request));
    strcpy(request.filename, filename);

    if (tags && tags[0] != '\0')
    {
        size_t tag_len = strlen(tags);
        if (tag_len < 160)
        {
            strcpy(request.tags, tags);
        }
    }

    notify_write_data(&request, sizeof(request));
    event_id_t event_id = notify_execute();
    if (event_id == 0)
    {
        return -1;
    }

    result_entry_t result;
    if (!result_wait(&result, 5000))
    {
        return -1;
    }

    if (result.error_code != BOX_OK)
    {
        return -1;
    }

    if (result.size < 8)
    {
        return -1;
    }

    uint32_t file_id, error_code;
    memcpy(&file_id, result.payload, 4);
    memcpy(&error_code, result.payload + 4, 4);

    if (error_code != 0)
    {
        return -1;
    }

    return (int)file_id;
}

int query(const char *tags, uint32_t *file_ids, size_t max_files)
{
    if (!file_ids || max_files == 0)
    {
        return -1;
    }

    notify_prepare();
    notify_add_prefix(STORAGE_DECK_ID, STORAGE_TAG_QUERY);

    char tag_buffer[192];
    memset(tag_buffer, 0, 192);

    if (tags && tags[0] != '\0')
    {
        size_t tag_len = strlen(tags);
        if (tag_len < 192)
        {
            strcpy(tag_buffer, tags);
        }
    }

    notify_write_data(tag_buffer, 192);
    event_id_t event_id = notify_execute();
    if (event_id == 0)
    {
        return -1;
    }

    result_entry_t result;
    if (!result_wait(&result, 5000))
    {
        return -1;
    }

    if (result.error_code != BOX_OK)
    {
        return -1;
    }

    // Result format: count[4] + file_id[4] + filename[32] (repeated)
    if (result.size < 4)
    {
        return -1;
    }

    uint32_t count;
    memcpy(&count, result.payload, 4);

    if (count > max_files)
    {
        count = max_files;
    }

    // Extract file IDs
    for (uint32_t i = 0; i < count; i++)
    {
        size_t offset = 4 + (i * 4); // 4 bytes count + i * 4 bytes file_id
        if (offset + 4 <= result.size)
        {
            memcpy(&file_ids[i], result.payload + offset, 4);
        }
    }

    return (int)count;
}

int file_info(uint32_t file_id, file_info_t *info)
{
    if (!info)
    {
        return -1;
    }

    notify_prepare();
    notify_add_prefix(STORAGE_DECK_ID, STORAGE_OBJ_GET_INFO);

    struct __attribute__((packed))
    {
        uint32_t file_id;
        uint8_t reserved[188];
    } request;

    memset(&request, 0, sizeof(request));
    request.file_id = file_id;

    notify_write_data(&request, sizeof(request));
    event_id_t event_id = notify_execute();
    if (event_id == 0)
    {
        return -1;
    }

    result_entry_t result;
    if (!result_wait(&result, 5000))
    {
        return -1;
    }

    if (result.error_code != BOX_OK)
    {
        return -1;
    }

    if (result.size < 192)
    {
        return -1;
    }

    // Parse obj_get_info_response_t
    int32_t error_code;
    memcpy(&error_code, result.payload, 4);

    if (error_code != 0)
    {
        return -1;
    }

    memcpy(&info->file_id, result.payload + 4, 4);
    memcpy(&info->flags, result.payload + 8, 4);
    memcpy(&info->size, result.payload + 12, 8);
    memcpy(&info->tag_count, result.payload + 20, 1);

    memcpy(info->filename, result.payload + 24, 32);

    // Parse tags (5 tags max, each 24 bytes: type[1] + key[11] + value[12])
    uint8_t tag_count = (info->tag_count > 5) ? 5 : info->tag_count;
    for (uint8_t i = 0; i < tag_count; i++)
    {
        size_t tag_offset = 56 + (i * 24);
        memcpy(&info->tags[i].type, result.payload + tag_offset, 1);
        memcpy(info->tags[i].key, result.payload + tag_offset + 1, 11);
        memcpy(info->tags[i].value, result.payload + tag_offset + 12, 12);
    }

    return 0;
}

int fread(uint32_t file_id, uint64_t offset, void *buffer, size_t size)
{
    if (!buffer || size == 0)
    {
        return -1;
    }

    if (size > 176)
    {
        size = 176;
    }

    notify_prepare();
    notify_add_prefix(STORAGE_DECK_ID, STORAGE_OBJ_READ);

    struct __attribute__((packed))
    {
        uint32_t file_id;
        uint64_t offset;
        uint32_t length;
        uint8_t reserved[176];
    } request;

    memset(&request, 0, sizeof(request));
    request.file_id = file_id;
    request.offset = offset;
    request.length = (uint32_t)size;

    notify_write_data(&request, sizeof(request));
    event_id_t event_id = notify_execute();
    if (event_id == 0)
    {
        return -1;
    }

    result_entry_t result;
    if (!result_wait(&result, 5000))
    {
        return -1;
    }

    if (result.error_code != BOX_OK)
    {
        return -1;
    }

    if (result.size < 16)
    {
        return -1;
    }

    uint64_t bytes_read;
    uint32_t error_code;
    memcpy(&bytes_read, result.payload, 8);
    memcpy(&error_code, result.payload + 8, 4);

    if (error_code != 0)
    {
        return -1;
    }

    if (bytes_read > 0)
    {
        if (bytes_read > size)
        {
            bytes_read = size;
        }

        if (result.size < 16 + bytes_read)
        {
            return -1;
        }

        memcpy(buffer, result.payload + 16, bytes_read);
    }

    return (int)bytes_read;
}

int fwrite(uint32_t file_id, uint64_t offset, const void *buffer, size_t size)
{
    if (!buffer || size == 0)
    {
        return -1;
    }

    if (size > 168)
    {
        size = 168;
    }

    notify_prepare();
    notify_add_prefix(STORAGE_DECK_ID, STORAGE_OBJ_WRITE);

    struct __attribute__((packed))
    {
        uint32_t file_id;
        uint64_t offset;
        uint32_t length;
        uint32_t flags;
        uint8_t data[168];
        uint8_t reserved[4];
    } request;

    memset(&request, 0, sizeof(request));
    request.file_id = file_id;
    request.offset = offset;
    request.length = (uint32_t)size;
    request.flags = 0;
    memcpy(request.data, buffer, size);

    notify_write_data(&request, sizeof(request));
    event_id_t event_id = notify_execute();
    if (event_id == 0)
    {
        return -1;
    }

    result_entry_t result;
    if (!result_wait(&result, 5000))
    {
        return -1;
    }

    if (result.error_code != BOX_OK)
    {
        return -1;
    }

    if (result.size < 20)
    {
        return -1;
    }

    uint64_t bytes_written;
    uint32_t error_code;
    memcpy(&bytes_written, result.payload, 8);
    memcpy(&error_code, result.payload + 16, 4);

    if (error_code != 0)
    {
        return -1;
    }

    return (int)bytes_written;
}

int file_rename(uint32_t file_id, const char *new_filename)
{
    if (!new_filename)
    {
        return -1;
    }

    size_t fn_len = strlen(new_filename);
    if (fn_len == 0 || fn_len >= 32)
    {
        return -1;
    }

    notify_prepare();
    notify_add_prefix(STORAGE_DECK_ID, STORAGE_OBJ_RENAME);

    struct __attribute__((packed))
    {
        uint32_t file_id;
        char new_filename[32];
        uint8_t reserved[156];
    } request;

    memset(&request, 0, sizeof(request));
    request.file_id = file_id;
    strcpy(request.new_filename, new_filename);

    notify_write_data(&request, sizeof(request));
    event_id_t event_id = notify_execute();
    if (event_id == 0)
    {
        return -1;
    }

    result_entry_t result;
    if (!result_wait(&result, 5000))
    {
        return -1;
    }

    if (result.error_code != BOX_OK)
    {
        return -1;
    }

    if (result.size < 4)
    {
        return -1;
    }

    int32_t error_code;
    memcpy(&error_code, result.payload, 4);

    return (error_code == 0) ? 0 : -1;
}

int delete(uint32_t file_id)
{
    notify_prepare();
    notify_add_prefix(STORAGE_DECK_ID, STORAGE_OBJ_DELETE);

    struct __attribute__((packed))
    {
        uint32_t file_id;
        uint8_t reserved[188];
    } request;

    memset(&request, 0, sizeof(request));
    request.file_id = file_id;

    notify_write_data(&request, sizeof(request));
    event_id_t event_id = notify_execute();
    if (event_id == 0)
    {
        return -1;
    }

    result_entry_t result;
    if (!result_wait(&result, 5000))
    {
        return -1;
    }

    if (result.error_code != BOX_OK)
    {
        return -1;
    }

    if (result.size < 4)
    {
        return -1;
    }

    int32_t error_code;
    memcpy(&error_code, result.payload, 4);

    return (error_code == 0) ? 0 : -1;
}

// ============================================================================
// TAG OPERATIONS
// ============================================================================

int tag_add(uint32_t file_id, const char *tag)
{
    if (!tag || tag[0] == '\0')
    {
        return -1;
    }

    size_t tag_len = strlen(tag);
    if (tag_len >= 32)
    {
        return -1;
    }

    notify_prepare();
    notify_add_prefix(STORAGE_DECK_ID, STORAGE_TAG_SET);

    struct __attribute__((packed))
    {
        uint32_t file_id;
        char tag[32];
        uint8_t reserved[156];
    } request;

    memset(&request, 0, sizeof(request));
    request.file_id = file_id;
    strcpy(request.tag, tag);

    notify_write_data(&request, sizeof(request));
    event_id_t event_id = notify_execute();
    if (event_id == 0)
    {
        return -1;
    }

    result_entry_t result;
    if (!result_wait(&result, 5000))
    {
        return -1;
    }

    if (result.error_code != BOX_OK)
    {
        return -1;
    }

    return 0;
}

int tag_remove(uint32_t file_id, const char *key)
{
    if (!key || key[0] == '\0')
    {
        return -1;
    }

    size_t key_len = strlen(key);
    if (key_len >= 32)
    {
        return -1;
    }

    notify_prepare();
    notify_add_prefix(STORAGE_DECK_ID, STORAGE_TAG_UNSET);

    struct __attribute__((packed))
    {
        uint32_t file_id;
        char tag[32];
        uint8_t reserved[156];
    } request;

    memset(&request, 0, sizeof(request));
    request.file_id = file_id;
    strcpy(request.tag, key);

    notify_write_data(&request, sizeof(request));
    event_id_t event_id = notify_execute();
    if (event_id == 0)
    {
        return -1;
    }

    result_entry_t result;
    if (!result_wait(&result, 5000))
    {
        return -1;
    }

    if (result.error_code != BOX_OK)
    {
        return -1;
    }

    return 0;
}

// ============================================================================
// CONTEXT OPERATIONS
// ============================================================================

int context_set(const char *tag)
{
    if (!tag || tag[0] == '\0')
    {
        return -1;
    }

    size_t tag_len = strlen(tag);
    if (tag_len >= 32)
    {
        return -1;
    }

    notify_prepare();
    notify_add_prefix(STORAGE_DECK_ID, STORAGE_CONTEXT_SET);

    char tag_buffer[192];
    memset(tag_buffer, 0, 192);
    strcpy(tag_buffer, tag);

    notify_write_data(tag_buffer, 192);
    event_id_t event_id = notify_execute();
    if (event_id == 0)
    {
        return -1;
    }

    result_entry_t result;
    if (!result_wait(&result, 5000))
    {
        return -1;
    }

    if (result.error_code != BOX_OK)
    {
        return -1;
    }

    return 0;
}

int context_clear(void)
{
    notify_prepare();
    notify_add_prefix(STORAGE_DECK_ID, STORAGE_CONTEXT_CLEAR);

    uint8_t dummy[192];
    memset(dummy, 0, 192);

    notify_write_data(dummy, 192);
    event_id_t event_id = notify_execute();
    if (event_id == 0)
    {
        return -1;
    }

    result_entry_t result;
    if (!result_wait(&result, 5000))
    {
        return -1;
    }

    if (result.error_code != BOX_OK)
    {
        return -1;
    }

    return 0;
}

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

int fwrite_all(uint32_t file_id, const void *buffer, size_t total_size)
{
    const uint8_t *src = (const uint8_t *)buffer;
    size_t written = 0;
    const size_t chunk_size = 168;

    while (written < total_size)
    {
        size_t to_write = total_size - written;
        if (to_write > chunk_size)
        {
            to_write = chunk_size;
        }

        int result = fwrite(file_id, written, src + written, to_write);
        if (result < 0)
        {
            return -1;
        }

        written += result;
        if ((size_t)result < to_write)
        {
            break;
        }
    }

    return (int)written;
}

int fread_all(uint32_t file_id, void *buffer, size_t max_size, size_t *bytes_read)
{
    uint8_t *dest = (uint8_t *)buffer;
    size_t total_read = 0;
    const size_t chunk_size = 168;

    while (total_read < max_size)
    {
        size_t to_read = max_size - total_read;
        if (to_read > chunk_size)
        {
            to_read = chunk_size;
        }

        int result = fread(file_id, total_read, dest + total_read, to_read);
        if (result < 0)
        {
            return -1;
        }

        total_read += result;

        if (result == 0 || (size_t)result < to_read)
        {
            break;
        }
    }

    if (bytes_read)
    {
        *bytes_read = total_read;
    }

    return 0;
}
