#include "storage_test.h"
#include "storage_deck.h"
#include "tagfs.h"
#include "klib.h"
#include "process.h"
#include "guide.h"
#include "events.h"

static void test_tag_query(void) {
    kprintf("\n[TEST 1] TAG_QUERY for kernel file\n");

    const char* query_tags[] = {"type:kernel"};
    uint32_t file_ids[100];

    int count = tagfs_query_files(query_tags, 1, file_ids, 100);
    if (count >= 0) {
        debug_printf("[TEST 1] PASS: Found %d file(s) with tag 'type:kernel'\n", count);
        if (count > 0) {
            debug_printf("[TEST 1]   File ID: %u\n", file_ids[0]);
        }
    } else {
        debug_printf("[TEST 1] FAIL: Query returned error %d\n", count);
    }
}

static void test_file_metadata(void) {
    kprintf("\n[TEST 2] Read kernel file metadata\n");

    TagFSMetadata* meta = tagfs_get_metadata(1);
    if (!meta || !(meta->flags & TAGFS_FILE_ACTIVE)) {
        debug_printf("[TEST 2] FAIL: Could not read metadata for file 1\n");
        return;
    }

    debug_printf("[TEST 2] PASS: Metadata read successfully\n");
    debug_printf("[TEST 2]   File ID: %u\n", meta->file_id);
    debug_printf("[TEST 2]   Filename: %s\n", meta->filename);
    debug_printf("[TEST 2]   Size: %llu bytes\n", meta->size);
    debug_printf("[TEST 2]   Blocks: %u\n", meta->block_count);
    debug_printf("[TEST 2]   Tags: %u\n", meta->tag_count);

    for (uint32_t i = 0; i < meta->tag_count; i++) {
        if (meta->tags[i].type == TAGFS_TAG_SYSTEM) {
            debug_printf("[TEST 2]     - %s (system)\n", meta->tags[i].key);
        } else {
            debug_printf("[TEST 2]     - %s:%s\n", meta->tags[i].key, meta->tags[i].value);
        }
    }
}

static void test_file_open_read(void) {
    kprintf("\n[TEST 3] Open and read kernel file\n");

    TagFSFileHandle* handle = tagfs_open(1, TAGFS_HANDLE_READ);
    if (!handle) {
        debug_printf("[TEST 3] FAIL: Could not open file 1\n");
        return;
    }

    uint8_t buffer[256];
    int bytes_read = tagfs_read(handle, buffer, 256);

    if (bytes_read > 0) {
        debug_printf("[TEST 3] PASS: Read %d bytes from kernel file\n", bytes_read);
        debug_printf("[TEST 3]   First 16 bytes (hex): ");
        for (int i = 0; i < 16 && i < bytes_read; i++) {
            kprintf("%02x ", buffer[i]);
        }
        kprintf("\n");
    } else {
        debug_printf("[TEST 3] FAIL: Read returned %d\n", bytes_read);
    }

    tagfs_close(handle);
}

static void test_tag_index_stats(void) {
    kprintf("\n[TEST 4] Tag index statistics\n");

    TagFSState* state = tagfs_get_state();
    if (!state || !state->tag_index) {
        debug_printf("[TEST 4] FAIL: Tag index not initialized\n");
        return;
    }

    debug_printf("[TEST 4] PASS: Tag index operational\n");
    debug_printf("[TEST 4]   Total unique tags: %u\n", state->tag_index->total_tags);
    debug_printf("[TEST 4]   Filesystem version: %u\n", state->superblock.version);
    debug_printf("[TEST 4]   Total files: %u\n", state->superblock.total_files);
    debug_printf("[TEST 4]   Free blocks: %u / %u\n",
            state->superblock.free_blocks,
            state->superblock.total_blocks);
}

static void test_file_growth(void) {
    kprintf("\n[TEST 5] File growth (CRITICAL FIX TEST)\n");

    debug_printf("[TEST 5] About to create file...\n");
    uint32_t file_id;
    // Create file with no tags to avoid potential tag parsing issues
    debug_printf("[TEST 5] Calling tagfs_create_file...\n");
    if (tagfs_create_file("growth_test.txt", NULL, 0, &file_id) != 0) {
        debug_printf("[TEST 5] FAIL: Could not create file\n");
        return;
    }
    debug_printf("[TEST 5] Created file %u\n", file_id);

    TagFSFileHandle* handle = tagfs_open(file_id, TAGFS_HANDLE_WRITE);
    if (!handle) {
        debug_printf("[TEST 5] FAIL: Could not open file\n");
        tagfs_delete_file(file_id);
        return;
    }

    char data1[100];
    memset(data1, 'A', 100);
    int written1 = tagfs_write(handle, data1, 100);
    debug_printf("[TEST 5] First write: %d bytes (expected 100)\n", written1);

    if (written1 != 100) {
        debug_printf("[TEST 5] FAIL: First write failed (returned %d)\n", written1);
        tagfs_close(handle);
        tagfs_delete_file(file_id);
        return;
    }

    char data2[4000];
    memset(data2, 'B', 4000);
    int written2 = tagfs_write(handle, data2, 4000);
    debug_printf("[TEST 5] Second write: %d bytes (expected 4000)\n", written2);

    if (written2 != 4000) {
        debug_printf("[TEST 5] FAIL: Second write failed (returned %d)\n", written2);
        tagfs_close(handle);
        tagfs_delete_file(file_id);
        return;
    }

    TagFSMetadata meta;
    tagfs_read_metadata(file_id, &meta);
    debug_printf("[TEST 5] Final size: %llu bytes, blocks: %u\n", meta.size, meta.block_count);

    if (meta.size == 4100 && meta.block_count == 2) {
        debug_printf("[TEST 5] PASS: File growth works correctly!\n");
    } else {
        debug_printf("[TEST 5] FAIL: Expected size=4100 blocks=2, got size=%llu blocks=%u\n",
                meta.size, meta.block_count);
    }

    tagfs_close(handle);
    tagfs_delete_file(file_id);
}

static void test_obj_create_basic(void) {
    kprintf("\n[TEST 6] OBJ_CREATE: Basic file creation\n");

    Event event;
    event_init(&event, 1, 200);
    event.prefixes[0] = 0x0207;
    event.prefix_count = 1;
    event.current_prefix_idx = 0;

    obj_create_event_t* req = (obj_create_event_t*)event.data;
    strncpy(req->filename, "test.txt", 32);
    req->tags[0] = '\0';

    storage_deck_handler(&event);

    obj_create_response_t* resp = (obj_create_response_t*)event.data;
    if (resp->error_code == STORAGE_OK && resp->file_id > 0) {
        debug_printf("[TEST 6] PASS: Created file_id=%u\n", resp->file_id);
        tagfs_delete_file(resp->file_id);
    } else {
        debug_printf("[TEST 6] FAIL: error_code=%d\n", resp->error_code);
    }
}

static void test_obj_write_read_cycle(void) {
    kprintf("\n[TEST 7] OBJ_WRITE/READ: Write and read back\n");

    Event event;
    event_init(&event, 1, 201);
    event.prefixes[0] = 0x0207;
    event.prefix_count = 1;
    event.current_prefix_idx = 0;

    obj_create_event_t* create_req = (obj_create_event_t*)event.data;
    strncpy(create_req->filename, "data.txt", 32);
    create_req->tags[0] = '\0';

    storage_deck_handler(&event);
    obj_create_response_t* create_resp = (obj_create_response_t*)event.data;
    uint32_t file_id = create_resp->file_id;

    debug_printf("[TEST 7]   Created file_id=%u\n", file_id);

    event_init(&event, 1, 202);
    event.prefixes[0] = 0x0206;
    event.prefix_count = 1;
    event.current_prefix_idx = 0;

    obj_write_event_t* write_req = (obj_write_event_t*)event.data;
    write_req->file_id = file_id;
    write_req->offset = 0;
    write_req->length = 13;
    write_req->flags = 0;
    memcpy(write_req->data, "Hello, BoxOS!", 13);

    handle_obj_write(&event);
    obj_write_response_t* write_resp = (obj_write_response_t*)event.data;

    debug_printf("[TEST 7]   Wrote %llu bytes, file_size=%llu\n",
            write_resp->bytes_written, write_resp->new_file_size);

    event_init(&event, 1, 203);
    event.prefixes[0] = 0x0205;
    event.prefix_count = 1;
    event.current_prefix_idx = 0;

    obj_read_event_t* read_req = (obj_read_event_t*)event.data;
    read_req->file_id = file_id;
    read_req->offset = 0;
    read_req->length = 13;

    handle_obj_read(&event);
    obj_read_response_t* read_resp = (obj_read_response_t*)event.data;

    debug_printf("[TEST 7]   Read %llu bytes\n", read_resp->bytes_read);

    if (read_resp->bytes_read == 13 &&
        memcmp(read_resp->data, "Hello, BoxOS!", 13) == 0) {
        debug_printf("[TEST 7] PASS: Data matches!\n");
    } else {
        debug_printf("[TEST 7] FAIL: Data mismatch\n");
        debug_printf("[TEST 7]   Expected: Hello, BoxOS!\n");
        debug_printf("[TEST 7]   Got: ");
        for (uint64_t i = 0; i < read_resp->bytes_read && i < 13; i++) {
            kprintf("%c", read_resp->data[i]);
        }
        kprintf("\n");
    }
    tagfs_delete_file(file_id);
}

static void test_obj_write_append(void) {
    kprintf("\n[TEST 8] OBJ_WRITE: Append mode\n");

    Event event;
    event_init(&event, 1, 204);
    event.prefixes[0] = 0x0207;
    event.prefix_count = 1;
    event.current_prefix_idx = 0;

    obj_create_event_t* create_req = (obj_create_event_t*)event.data;
    strncpy(create_req->filename, "append.txt", 32);
    create_req->tags[0] = '\0';

    storage_deck_handler(&event);
    obj_create_response_t* create_resp = (obj_create_response_t*)event.data;
    uint32_t file_id = create_resp->file_id;

    event_init(&event, 1, 205);
    event.prefixes[0] = 0x0206;
    event.prefix_count = 1;
    event.current_prefix_idx = 0;

    obj_write_event_t*     write_req = (obj_write_event_t*)event.data;
    write_req->file_id = file_id;
    write_req->offset = 0;
    write_req->length = 5;
    write_req->flags = 0;
    memcpy(write_req->data, "Hello", 5);

    handle_obj_write(&event);

    event_init(&event, 1, 206);
    event.prefixes[0] = 0x0206;
    event.prefix_count = 1;
    event.current_prefix_idx = 0;

    write_req = (obj_write_event_t*)event.data;
    write_req->file_id = file_id;
    write_req->offset = 999;
    write_req->length = 7;
    write_req->flags = OBJ_WRITE_APPEND;
    memcpy(write_req->data, ", World", 7);

    handle_obj_write(&event);
    obj_write_response_t* write_resp = (obj_write_response_t*)event.data;

    debug_printf("[TEST 8]   Final size: %llu bytes\n", write_resp->new_file_size);

    event_init(&event, 1, 207);
    event.prefixes[0] = 0x0205;
    event.prefix_count = 1;
    event.current_prefix_idx = 0;

    obj_read_event_t* read_req = (obj_read_event_t*)event.data;
    read_req->file_id = file_id;
    read_req->offset = 0;
    read_req->length = 12;

    handle_obj_read(&event);
    obj_read_response_t* read_resp = (obj_read_response_t*)event.data;

    if (read_resp->bytes_read == 12 &&
        memcmp(read_resp->data, "Hello, World", 12) == 0) {
        debug_printf("[TEST 8] PASS: Append works correctly!\n");
    } else {
        debug_printf("[TEST 8] FAIL: Data mismatch\n");
    }
    tagfs_delete_file(file_id);
}

static void test_obj_error_handling(void) {
    kprintf("\n[TEST 9] Error handling\n");

    Event event;
    event_init(&event, 1, 208);
    event.prefixes[0] = 0x0205;
    event.prefix_count = 1;
    event.current_prefix_idx = 0;

    obj_read_event_t* read_req = (obj_read_event_t*)event.data;
    read_req->file_id = 9999;
    read_req->offset = 0;
    read_req->length = 10;

    storage_deck_handler(&event);
    obj_read_response_t* read_resp = (obj_read_response_t*)event.data;

    if ((int32_t)read_resp->error_code == STORAGE_ERR_NOT_FOUND) {
        debug_printf("[TEST 9] PASS: Invalid file_id correctly rejected\n");
    } else {
        debug_printf("[TEST 9] FAIL: Expected error_code=%d, got %d\n",
                STORAGE_ERR_NOT_FOUND, (int32_t)read_resp->error_code);
    }
}

void test_storage_deck(void) {
    kprintf("\n====================================\n");
    kprintf("STORAGE DECK & TAGFS TEST\n");
    kprintf("====================================\n");

    TagFSState* state = tagfs_get_state();
    if (!state || !state->initialized) {
        kprintf("FAIL: TagFS not initialized\n");
        return;
    }

    test_tag_query();
    test_file_metadata();
    test_file_open_read();
    test_tag_index_stats();
    test_file_growth();
    test_obj_create_basic();
    test_obj_write_read_cycle();
    test_obj_write_append();
    test_obj_error_handling();

    kprintf("\n====================================\n");
    kprintf("STORAGE DECK TEST COMPLETE\n");
    kprintf("====================================\n");
}
