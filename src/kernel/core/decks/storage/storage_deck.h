#ifndef STORAGE_DECK_H
#define STORAGE_DECK_H

#include "ktypes.h"
#include "pocket.h"
#include "boxos_decks.h"

#define STORAGE_DECK_ID DECK_STORAGE

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

// Storage Deck error codes are defined in error.h

typedef struct __packed
{
    char filename[32];
    char tags[160];
} obj_create_event_t;

typedef struct __packed
{
    uint32_t file_id;
    uint32_t error_code;
    uint8_t reserved[184];
} obj_create_response_t;

typedef struct __packed
{
    uint32_t file_id;
    uint64_t offset;
    uint32_t length;
    uint32_t flags;
    uint8_t data[168];
    uint8_t reserved[4];
} obj_write_event_t;

typedef struct __packed
{
    uint64_t bytes_written;
    uint64_t new_file_size;
    uint32_t error_code;
    uint8_t reserved[172];
} obj_write_response_t;

typedef struct __packed
{
    uint32_t file_id;
    uint64_t offset;
    uint32_t length;
    uint8_t reserved[176];
} obj_read_event_t;

typedef struct __packed
{
    uint64_t bytes_read;
    uint32_t error_code;
    uint8_t reserved[4];
    uint8_t data[176];
} obj_read_response_t;

#define OBJ_WRITE_APPEND (1 << 0)

typedef struct __packed
{
    uint32_t file_id;
    char new_filename[32];
    uint8_t reserved[156];
} obj_rename_request_t;

typedef struct __packed
{
    int32_t error_code;
    uint8_t reserved[188];
} obj_rename_response_t;

STATIC_ASSERT(sizeof(obj_rename_request_t) == 192, "obj_rename_request_t must be 192 bytes");
STATIC_ASSERT(sizeof(obj_rename_response_t) == 192, "obj_rename_response_t must be 192 bytes");

typedef struct __packed
{
    uint32_t file_id;
    uint8_t reserved[188];
} obj_delete_request_t;

typedef struct __packed
{
    int32_t error_code;
    uint8_t reserved[188];
} obj_delete_response_t;

STATIC_ASSERT(sizeof(obj_delete_request_t) == 192, "obj_delete_request_t must be 192 bytes");
STATIC_ASSERT(sizeof(obj_delete_response_t) == 192, "obj_delete_response_t must be 192 bytes");

typedef struct __packed
{
    uint32_t file_id;
    uint8_t reserved[188];
} obj_get_info_request_t;

typedef struct __packed
{
    int32_t error_code;
    uint32_t file_id;
    uint32_t flags;
    uint64_t size;
    uint8_t tag_count;
    uint8_t reserved1[3];
    char filename[32];

    struct __packed
    {
        uint8_t type;
        char key[11];
        char value[12];
    } tags[5];

    uint8_t reserved2[16];
} obj_get_info_response_t;

STATIC_ASSERT(sizeof(obj_get_info_request_t) == 192, "obj_get_info_request_t must be 192 bytes");
STATIC_ASSERT(sizeof(obj_get_info_response_t) == 192, "obj_get_info_response_t must be 192 bytes");

void storage_deck_init(void);
int storage_deck_handler(Pocket *event);

// Synchronous I/O handlers (bypass async path)
int handle_obj_read(Pocket *event);
int handle_obj_write(Pocket *event);

#endif // STORAGE_DECK_H
