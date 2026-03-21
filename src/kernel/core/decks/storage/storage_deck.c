#include "storage_deck.h"
#include "pocket.h"
#include "vmm.h"
#include "process.h"
#include "tagfs.h"
#include "tag_bitmap.h"
#include "tag_registry.h"
#include "tagfs_context.h"
#include "klib.h"
#include "ata_dma.h"
#include "ata.h"
#include "async_io.h"
#include "kernel_config.h"
#include "atomics.h"
#include "deck_utils.h"

static int parse_tag_list(const char *tag_string, const char *tags[], uint32_t max_tags, char buffer[16][32]);
int handle_obj_read(Pocket *pocket, process_t *proc);
int handle_obj_write(Pocket *pocket, process_t *proc);

static atomic_u32_t async_id_counter = 0;

static inline uint32_t next_async_id(void)
{
    return atomic_fetch_add_u32(&async_id_counter, 1);
}

// Resolve pocket->data_addr to a kernel-accessible pointer.
// Uses proc directly; falls back to process_find if proc is NULL.
// Returns NULL if process not found, address not mapped, or data_length is 0.
static void *pocket_data_ptr(Pocket *pocket, process_t *proc)
{
    if (!pocket || pocket->data_length == 0 || pocket->data_addr == 0)
        return NULL;
    if (!proc)
    {
        proc = process_find(pocket->pid);
        if (!proc)
            return NULL;
    }
    return vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
}

void storage_deck_init(void)
{
    debug_printf("[Storage Deck] Initializing...\n");

    if (tagfs_init() != 0)
    {
        debug_printf("[Storage Deck] ERROR: Failed to initialize TagFS\n");
        return;
    }

    tagfs_context_init();
    debug_printf("[Storage Deck] Initialization complete\n");
}

static int handle_tag_query(Pocket *pocket, process_t *proc)
{
    TagFSState *state = tagfs_get_state();
    if (!state->initialized)
    {
        return ERR_INVALID_ARGUMENT;
    }

    spin_lock(&state->lock);

    uint8_t *data = pocket_data_ptr(pocket, proc);
    if (!data)
    {
        spin_unlock(&state->lock);
        return ERR_INVALID_ARGUMENT;
    }

    const char *tag_string = (const char *)data;
    uint32_t pid = pocket->pid;

    uint32_t max_results = (pocket->data_length - sizeof(uint32_t)) / sizeof(uint32_t);
    if (pocket->data_length <= sizeof(uint32_t))
        max_results = 0;

    uint32_t *file_ids = kmalloc(max_results * sizeof(uint32_t));
    if (!file_ids)
    {
        deck_write_u32(data, 0);
        spin_unlock(&state->lock);
        return OK;
    }

    int count = 0;

    const char *ctx_tags[64];
    int context_count = tagfs_context_get_tags(pid, ctx_tags, 64);

    const char *all_tags[32];
    int total_tag_count = 0;

    if (tag_string && tag_string[0] != '\0')
    {
        const char *query_tags[16];
        char tag_buffer[16][32];
        int query_count = parse_tag_list(tag_string, query_tags, 16, tag_buffer);

        for (int i = 0; i < query_count && total_tag_count < 32; i++)
        {
            all_tags[total_tag_count++] = query_tags[i];
        }
    }

    for (int i = 0; i < context_count && total_tag_count < 32; i++)
    {
        all_tags[total_tag_count++] = ctx_tags[i];
    }

    if (total_tag_count > 0)
    {
        count = tagfs_query_files(all_tags, (uint32_t)total_tag_count,
                                  file_ids, max_results);
        if (count < 0)
        {
            count = 0;
        }
    }
    else
    {
        count = tagfs_list_all_files(file_ids, max_results);
    }

    deck_write_u32(data, (uint32_t)count);
    memcpy(data + sizeof(uint32_t), file_ids, count * sizeof(uint32_t));
    kfree(file_ids);

    spin_unlock(&state->lock);
    return OK;
}

static int handle_tag_set(Pocket *pocket, process_t *proc)
{
    TagFSState *state = tagfs_get_state();
    if (!state->initialized)
    {
        return ERR_INVALID_ARGUMENT;
    }

    uint8_t *data = pocket_data_ptr(pocket, proc);
    if (!data)
    {
        return ERR_INVALID_ARGUMENT;
    }

    uint32_t file_id = deck_read_u32(data);
    const char *tag_string = (const char *)(data + sizeof(uint32_t));

    char key[64];
    char value[64];

    if (tagfs_parse_tag(tag_string, key, sizeof(key), value, sizeof(value)) != 0)
    {
        return ERR_INVALID_ARGUMENT;
    }

    if (tagfs_add_tag_string(file_id, key, value[0] ? value : NULL) != 0)
    {
        return ERR_INVALID_ARGUMENT;
    }

    return OK;
}

static int handle_tag_unset(Pocket *pocket, process_t *proc)
{
    TagFSState *state = tagfs_get_state();
    if (!state->initialized)
    {
        return ERR_INVALID_ARGUMENT;
    }

    uint8_t *data = pocket_data_ptr(pocket, proc);
    if (!data)
    {
        return ERR_INVALID_ARGUMENT;
    }

    uint32_t file_id = deck_read_u32(data);
    const char *key = (const char *)(data + sizeof(uint32_t));

    if (tagfs_remove_tag_string(file_id, key) != 0)
    {
        return ERR_INVALID_ARGUMENT;
    }

    return OK;
}

static int parse_tag_list(const char *tag_string, const char *tags[], uint32_t max_tags, char buffer[16][32])
{
    if (!tag_string || !tags || tag_string[0] == '\0')
    {
        return 0;
    }

    uint32_t count = 0;
    const char *pos = tag_string;

    while (*pos != '\0' && count < max_tags)
    {
        size_t len = 0;
        while (pos[len] != ',' && pos[len] != '\0' && len < 31)
        {
            len++;
        }

        if (len > 0)
        {
            for (size_t i = 0; i < len; i++)
            {
                buffer[count][i] = pos[i];
            }
            buffer[count][len] = '\0';
            tags[count] = buffer[count];
            count++;
        }

        pos += len;
        // Skip any remaining characters of an oversized tag name
        while (*pos != ',' && *pos != '\0')
        {
            pos++;
        }
        if (*pos == ',')
        {
            pos++;
        }
    }

    return count;
}

#ifdef CONFIG_ATA_DMA_ASYNC
static int handle_obj_read_async(Pocket *pocket, process_t *proc)
{
    uint8_t *data = pocket_data_ptr(pocket, proc);
    if (!data)
    {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return ERR_INVALID_ARGUMENT;
    }

    obj_read_event_t *req = (obj_read_event_t *)data;
    obj_read_response_t *resp = (obj_read_response_t *)data;

    uint32_t file_id = req->file_id;
    uint64_t offset = req->offset;
    uint32_t length = req->length;

    TagFSFileHandle *handle = tagfs_open(file_id, TAGFS_HANDLE_READ);
    if (!handle)
    {
        resp->error_code = ERR_FILE_NOT_FOUND;
        pocket->error_code = ERR_FILE_NOT_FOUND;
        return ERR_FILE_NOT_FOUND;
    }

    uint32_t start_block = (handle->extent_count > 0) ? handle->extents[0].start_block : 0;
    tagfs_close(handle);

    uint32_t lba = start_block * 8 + (uint32_t)(offset / 512);
    uint16_t sector_count = (length + 511) / 512;

    async_io_request_t io_req;
    io_req.event_id = next_async_id();
    io_req.pid = pocket->pid;
    io_req.lba = lba;
    io_req.sector_count = sector_count;
    io_req.is_master = 1;
    io_req.op = ASYNC_IO_OP_READ;
    io_req.buffer_virt = NULL;
    io_req.submit_time = rdtsc();

    error_t rc = async_io_submit(&io_req);

    if (rc == OK)
    {
        pocket->error_code = ERR_IO_PENDING;
        return OK;
    }
    else if (rc == ERR_IO_QUEUE_FULL)
    {
        debug_printf("[Storage Deck] Async queue full, falling back to sync OBJ_READ\n");
        return handle_obj_read(pocket, proc);
    }
    else
    {
        resp->error_code = ERR_IO;
        pocket->error_code = rc;
        return ERR_IO;
    }
}

static int handle_obj_write_async(Pocket *pocket, process_t *proc)
{
    uint8_t *data = pocket_data_ptr(pocket, proc);
    if (!data)
    {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return ERR_INVALID_ARGUMENT;
    }

    obj_write_event_t *req = (obj_write_event_t *)data;
    obj_write_response_t *resp = (obj_write_response_t *)data;

    uint32_t file_id = req->file_id;
    uint64_t offset = req->offset;
    uint32_t length = req->length;
    uint32_t flags = req->flags;

    TagFSFileHandle *handle = tagfs_open(file_id, TAGFS_HANDLE_WRITE);
    if (!handle)
    {
        resp->error_code = ERR_FILE_NOT_FOUND;
        pocket->error_code = ERR_FILE_NOT_FOUND;
        return ERR_FILE_NOT_FOUND;
    }

    uint32_t start_block = (handle->extent_count > 0) ? handle->extents[0].start_block : 0;
    uint64_t original_file_size = handle->file_size;

    uint64_t write_offset;
    if (flags & OBJ_WRITE_APPEND)
    {
        write_offset = original_file_size;
    }
    else
    {
        write_offset = offset;
    }

    tagfs_close(handle);

    uint32_t lba = start_block * 8 + (uint32_t)(write_offset / 512);
    uint16_t sector_count = (length + 511) / 512;

    async_io_request_t areq;
    areq.event_id = next_async_id();
    areq.pid = pocket->pid;
    areq.lba = lba;
    areq.sector_count = sector_count;
    areq.is_master = 1;
    areq.op = ASYNC_IO_OP_WRITE;
    areq.buffer_virt = req->data;
    areq.data_length = length;
    areq.submit_time = rdtsc();

    areq.file_id = file_id;
    areq.write_offset = write_offset;
    areq.original_file_size = original_file_size;

    error_t rc = async_io_submit(&areq);

    if (rc == OK)
    {
        pocket->error_code = ERR_IO_PENDING;
        return OK;
    }
    else if (rc == ERR_IO_QUEUE_FULL)
    {
        debug_printf("[Storage Deck] Async queue full, falling back to sync OBJ_WRITE\n");
        return handle_obj_write(pocket, proc);
    }
    else
    {
        resp->error_code = ERR_IO;
        pocket->error_code = rc;
        return ERR_IO;
    }
}
#endif

int handle_obj_read(Pocket *pocket, process_t *proc)
{
    uint8_t *data = pocket_data_ptr(pocket, proc);
    if (!data)
    {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return ERR_INVALID_ARGUMENT;
    }

    obj_read_event_t *req = (obj_read_event_t *)data;
    obj_read_response_t *resp = (obj_read_response_t *)data;

    uint32_t file_id = req->file_id;
    uint64_t offset = req->offset;
    uint32_t length = req->length;

    TagFSFileHandle *handle = tagfs_open(file_id, TAGFS_HANDLE_READ);
    if (!handle)
    {
        resp->error_code = ERR_FILE_NOT_FOUND;
        pocket->error_code = ERR_FILE_NOT_FOUND;
        return ERR_FILE_NOT_FOUND;
    }

    handle->offset = offset;

    uint8_t read_buffer[176];
    int bytes_read = tagfs_read(handle, read_buffer, length);

    tagfs_close(handle);

    memset(resp, 0, sizeof(obj_read_response_t));
    if (bytes_read >= 0)
    {
        resp->bytes_read = bytes_read;
        resp->error_code = OK;
        memcpy(resp->data, read_buffer, bytes_read);
        return OK;
    }
    else
    {
        resp->bytes_read = 0;
        resp->error_code = ERR_IO;
        pocket->error_code = ERR_IO;
        return ERR_IO;
    }
}

int handle_obj_write(Pocket *pocket, process_t *proc)
{
    uint8_t *data = pocket_data_ptr(pocket, proc);
    if (!data)
    {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return ERR_INVALID_ARGUMENT;
    }

    obj_write_event_t *req = (obj_write_event_t *)data;
    obj_write_response_t *resp = (obj_write_response_t *)data;

    uint32_t file_id = req->file_id;
    uint64_t offset = req->offset;
    uint32_t length = req->length;
    uint32_t flags = req->flags;

    size_t max_data_size = pocket->data_length - offsetof(obj_write_event_t, data);
    if (pocket->data_length < offsetof(obj_write_event_t, data) || length > max_data_size)
    {
        debug_printf("[StorageDeck] ERROR: Write length %u exceeds max data size %zu\n",
                     length, max_data_size);
        resp->error_code = ERR_INVALID_ARGUMENT;
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return ERR_INVALID_ARGUMENT;
    }

    uint8_t write_buffer[168];
    memcpy(write_buffer, req->data, length);

    TagFSFileHandle *handle = tagfs_open(file_id, TAGFS_HANDLE_WRITE);
    if (!handle)
    {
        resp->error_code = ERR_FILE_NOT_FOUND;
        pocket->error_code = ERR_FILE_NOT_FOUND;
        return ERR_FILE_NOT_FOUND;
    }

    if (flags & OBJ_WRITE_APPEND)
    {
        handle->offset = handle->file_size;
    }
    else
    {
        handle->offset = offset;
    }

    int written = tagfs_write(handle, write_buffer, length);

    uint64_t final_size = handle->file_size;

    tagfs_close(handle);

    memset(resp, 0, sizeof(obj_write_response_t));
    if (written >= 0)
    {
        resp->bytes_written = written;
        resp->new_file_size = final_size;
        resp->error_code = OK;
        return OK;
    }
    else
    {
        resp->bytes_written = 0;
        resp->new_file_size = final_size;
        resp->error_code = ERR_IO;
        pocket->error_code = ERR_IO;
        return ERR_IO;
    }
}

static int handle_obj_create(Pocket *pocket, process_t *proc)
{
    uint8_t *data = pocket_data_ptr(pocket, proc);
    if (!data)
    {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return ERR_INVALID_ARGUMENT;
    }

    obj_create_event_t *req = (obj_create_event_t *)data;

    if (req->filename[0] == '\0')
    {
        obj_create_response_t *resp = (obj_create_response_t *)data;
        resp->file_id = 0;
        resp->error_code = ERR_INVALID_ARGUMENT;
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return ERR_INVALID_ARGUMENT;
    }

    req->filename[31] = '\0';
    req->tags[159] = '\0';

    const char *tag_ptrs[16];
    char tag_buffer[16][32];
    uint32_t tag_count = 0;

    /* Auto-label tag is now handled by tagfs_create_file() -- no manual stem here */

    if (req->tags[0] != '\0')
    {
        tag_count += parse_tag_list(req->tags, tag_ptrs + tag_count, 16 - tag_count, tag_buffer + tag_count);
    }

    const char *ctx_tags[64];
    int ctx_count = tagfs_context_get_tags(pocket->pid, ctx_tags, 64);
    for (int i = 0; i < ctx_count && tag_count < 16; i++)
    {
        size_t clen = strlen(ctx_tags[i]);
        if (clen > 31)
            clen = 31;
        memcpy(tag_buffer[tag_count], ctx_tags[i], clen);
        tag_buffer[tag_count][clen] = '\0';
        tag_ptrs[tag_count] = tag_buffer[tag_count];
        tag_count++;
    }

    TagFSState *tfs_state = tagfs_get_state();
    uint16_t tag_id_buf[16];
    uint16_t intern_count = 0;
    if (tfs_state && tfs_state->registry)
    {
        for (uint32_t ti = 0; ti < tag_count && intern_count < 16; ti++)
        {
            char tkey[64];
            char tval[64];
            tagfs_parse_tag(tag_ptrs[ti], tkey, sizeof(tkey), tval, sizeof(tval));
            uint16_t tid = tag_registry_intern(tfs_state->registry, tkey,
                                               tval[0] ? tval : NULL);
            if (tid != TAGFS_INVALID_TAG_ID)
            {
                tag_id_buf[intern_count++] = tid;
            }
        }
    }

    uint32_t file_id;
    int result = tagfs_create_file(req->filename,
                                   intern_count > 0 ? tag_id_buf : NULL,
                                   intern_count, &file_id);

    obj_create_response_t *resp = (obj_create_response_t *)data;
    memset(resp, 0, sizeof(obj_create_response_t));
    if (result == 0)
    {
        resp->file_id = file_id;
        resp->error_code = OK;
        return OK;
    }
    else
    {
        resp->file_id = 0;
        resp->error_code = ERR_DISK_FULL;
        pocket->error_code = ERR_DISK_FULL;
        return ERR_DISK_FULL;
    }
}

static int handle_obj_delete(Pocket *pocket, process_t *proc)
{
    uint8_t *data = pocket_data_ptr(pocket, proc);
    if (!data)
    {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return ERR_INVALID_ARGUMENT;
    }

    obj_delete_request_t *req = (obj_delete_request_t *)data;
    uint32_t file_id = req->file_id;

    {
        TagFSMetadata del_meta;
        TagFSState *del_state = tagfs_get_state();
        if (tagfs_get_metadata(file_id, &del_meta) == 0 &&
            (del_meta.flags & TAGFS_FILE_ACTIVE))
        {
            for (uint16_t di = 0; di < del_meta.tag_count; di++)
            {
                const char *key = del_state ? tag_registry_key(del_state->registry, del_meta.tag_ids[di]) : NULL;
                const char *val = del_state ? tag_registry_value(del_state->registry, del_meta.tag_ids[di]) : NULL;
                if (!key)
                    continue;
                if (strcmp(key, "system") == 0 || strcmp(key, "boot") == 0)
                {
                    debug_printf("[Storage] ERROR: Cannot delete system file\n");
                    tagfs_metadata_free(&del_meta);
                    obj_delete_response_t *resp = (obj_delete_response_t *)data;
                    memset(resp, 0, sizeof(obj_delete_response_t));
                    resp->error_code = ERR_PERMISSION_DENIED;
                    pocket->error_code = ERR_PERMISSION_DENIED;
                    return ERR_PERMISSION_DENIED;
                }
                if (strcmp(key, "boot") == 0 && val && strcmp(val, "true") == 0)
                {
                    debug_printf("[Storage] ERROR: Cannot delete boot file\n");
                    tagfs_metadata_free(&del_meta);
                    obj_delete_response_t *resp = (obj_delete_response_t *)data;
                    memset(resp, 0, sizeof(obj_delete_response_t));
                    resp->error_code = ERR_PERMISSION_DENIED;
                    pocket->error_code = ERR_PERMISSION_DENIED;
                    return ERR_PERMISSION_DENIED;
                }
            }
            tagfs_metadata_free(&del_meta);
        }
    }

    int result = tagfs_delete_file(file_id);

    obj_delete_response_t *resp = (obj_delete_response_t *)data;
    memset(resp, 0, sizeof(obj_delete_response_t));

    if (result == 0)
    {
        resp->error_code = OK;
#if CONFIG_DEBUG_TAGFS
        debug_printf("[Storage] OBJ_DELETE: Success (file_id=%u)\n", file_id);
#endif
        return OK;
    }
    else
    {
        resp->error_code = ERR_FILE_NOT_FOUND;
        pocket->error_code = ERR_FILE_NOT_FOUND;
#if CONFIG_DEBUG_TAGFS
        debug_printf("[Storage] OBJ_DELETE: Failed (file_id=%u)\n", file_id);
#endif
        return ERR_FILE_NOT_FOUND;
    }
}

static int handle_obj_rename(Pocket *pocket, process_t *proc)
{
    uint8_t *data = pocket_data_ptr(pocket, proc);
    if (!data)
    {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return ERR_INVALID_ARGUMENT;
    }

    obj_rename_request_t *req = (obj_rename_request_t *)data;

    debug_printf("[Storage] OBJ_RENAME: file_id=%u, new_name='%s'\n",
                 req->file_id, req->new_filename);

    if (req->new_filename[0] == '\0')
    {
        debug_printf("[Storage] ERROR: Empty filename\n");
        obj_rename_response_t *resp = (obj_rename_response_t *)data;
        resp->error_code = ERR_INVALID_ARGUMENT;
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return ERR_INVALID_ARGUMENT;
    }

    int result = tagfs_rename_file(req->file_id, req->new_filename);

    obj_rename_response_t *resp = (obj_rename_response_t *)data;
    if (result == 0)
    {
        resp->error_code = OK;
        debug_printf("[Storage] OBJ_RENAME: Success\n");
    }
    else
    {
        resp->error_code = ERR_INVALID_ARGUMENT;
        pocket->error_code = ERR_INVALID_ARGUMENT;
        debug_printf("[Storage] OBJ_RENAME: Failed\n");
    }

    return result;
}

static int handle_obj_get_info(Pocket *pocket, process_t *proc)
{
    uint8_t *data = pocket_data_ptr(pocket, proc);
    if (!data)
    {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return ERR_INVALID_ARGUMENT;
    }

    obj_get_info_request_t *req = (obj_get_info_request_t *)data;

#if CONFIG_DEBUG_TAGFS
    debug_printf("[Storage] OBJ_GET_INFO: file_id=%u\n", req->file_id);
#endif

    TagFSMetadata metadata;
    if (tagfs_get_metadata(req->file_id, &metadata) != 0 ||
        !(metadata.flags & TAGFS_FILE_ACTIVE))
    {
        debug_printf("[Storage] ERROR: File %u not found\n", req->file_id);
        obj_get_info_response_t *resp = (obj_get_info_response_t *)data;
        resp->error_code = ERR_FILE_NOT_FOUND;
        pocket->error_code = ERR_FILE_NOT_FOUND;
        return ERR_FILE_NOT_FOUND;
    }

    obj_get_info_response_t *resp = (obj_get_info_response_t *)data;
    memset(resp, 0, sizeof(obj_get_info_response_t));

    resp->error_code = 0;
    resp->file_id = metadata.file_id;
    resp->flags = metadata.flags;
    resp->size = metadata.size;
    resp->tag_count = (metadata.tag_count > 5) ? 5 : (uint8_t)metadata.tag_count;

    if (metadata.filename)
    {
        strncpy(resp->filename, metadata.filename, 31);
        resp->filename[31] = '\0';
    }

    TagFSState *gi_state = tagfs_get_state();
    for (uint8_t i = 0; i < resp->tag_count; i++)
    {
        resp->tags[i].type = 0;
        const char *rkey = gi_state ? tag_registry_key(gi_state->registry, metadata.tag_ids[i]) : NULL;
        const char *rval = gi_state ? tag_registry_value(gi_state->registry, metadata.tag_ids[i]) : NULL;

        if (rkey)
        {
            strncpy(resp->tags[i].key, rkey, 10);
            resp->tags[i].key[10] = '\0';
        }
        if (rval)
        {
            strncpy(resp->tags[i].value, rval, 11);
            resp->tags[i].value[11] = '\0';
        }
    }

    // Synthesize FILE_FLAG_TRASHED from "trashed" tag presence.
    // This ensures flags & 0x02 is authoritative regardless of 5-tag limit.
    if (tagfs_has_tag_string(metadata.file_id, "trashed", NULL))
    {
        resp->flags |= 0x02; // FILE_FLAG_TRASHED
    }

    tagfs_metadata_free(&metadata);

#if CONFIG_DEBUG_TAGFS
    debug_printf("[Storage] OBJ_GET_INFO: Success (file='%s', tags=%u)\n",
                 resp->filename, resp->tag_count);
#endif
    return 0;
}

static int handle_context_set(Pocket *pocket, process_t *proc)
{
    uint8_t *data = pocket_data_ptr(pocket, proc);
    if (!data)
        return ERR_INVALID_ARGUMENT;

    const char *tag_string = (const char *)data;

    char key[64];
    char value[64];
    const char *colon = strchr(tag_string, ':');
    if (colon)
    {
        size_t klen = (size_t)(colon - tag_string);
        if (klen >= sizeof(key))
            klen = sizeof(key) - 1;
        memcpy(key, tag_string, klen);
        key[klen] = '\0';
        strncpy(value, colon + 1, sizeof(value) - 1);
        value[sizeof(value) - 1] = '\0';
    }
    else
    {
        strncpy(key, tag_string, sizeof(key) - 1);
        key[sizeof(key) - 1] = '\0';
        value[0] = '\0';
    }

    if (tagfs_context_add_tag_string(pocket->pid, key, value[0] ? value : NULL) != 0)
    {
        return ERR_INVALID_ARGUMENT;
    }

    return OK;
}

static int handle_context_clear(Pocket *pocket)
{
    tagfs_context_clear(pocket->pid);
    return OK;
}

int storage_deck_handler(Pocket *pocket, process_t *proc)
{
    if (!pocket_validate(pocket))
    {
        return ERR_INVALID_ARGUMENT;
    }

    uint16_t prefix = pocket_current_prefix(pocket);
    uint8_t deck_id = (prefix >> 8) & 0xFF;
    uint8_t opcode = prefix & 0xFF;

    if (deck_id != 0x02)
    {
        debug_printf("[Storage Deck] ERROR: Invalid deck_id: 0x%02x\n", deck_id);
        return ERR_INVALID_ARGUMENT;
    }

    int result = ERR_INVALID_ARGUMENT;

    switch (opcode)
    {
    case STORAGE_TAG_QUERY:
        result = handle_tag_query(pocket, proc);
        break;
    case STORAGE_TAG_SET:
        result = handle_tag_set(pocket, proc);
        break;
    case STORAGE_TAG_UNSET:
        result = handle_tag_unset(pocket, proc);
        break;
    case STORAGE_OBJ_READ:
#ifdef CONFIG_ATA_DMA_ASYNC
        result = handle_obj_read_async(pocket, proc);
#else
        result = handle_obj_read(pocket, proc);
#endif
        break;
    case STORAGE_OBJ_WRITE:
#ifdef CONFIG_ATA_DMA_ASYNC
        result = handle_obj_write_async(pocket, proc);
#else
        result = handle_obj_write(pocket, proc);
#endif
        break;
    case STORAGE_OBJ_CREATE:
        result = handle_obj_create(pocket, proc);
        break;
    case STORAGE_OBJ_DELETE:
        result = handle_obj_delete(pocket, proc);
        break;
    case STORAGE_OBJ_RENAME:
        result = handle_obj_rename(pocket, proc);
        break;
    case STORAGE_OBJ_GET_INFO:
        result = handle_obj_get_info(pocket, proc);
        break;
    case STORAGE_CONTEXT_SET:
        result = handle_context_set(pocket, proc);
        break;
    case STORAGE_CONTEXT_CLEAR:
        result = handle_context_clear(pocket);
        break;
    default:
        debug_printf("[Storage Deck] ERROR: Unknown opcode: 0x%02x\n", opcode);
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return ERR_INVALID_ARGUMENT;
    }

    if (result != OK)
    {
        pocket->error_code = (uint32_t)result;
    }

    return result;
}
