#ifndef STORAGE_DECK_H
#define STORAGE_DECK_H

#include "ktypes.h"
#include "events.h"
#include "boxos_decks.h"

// Storage Deck ID
#define STORAGE_DECK_ID DECK_STORAGE

// Storage Deck opcodes
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
// (STORAGE_OK, STORAGE_ERR_INVALID, STORAGE_ERR_NOT_FOUND, etc.)

// OBJ_CREATE structures
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

// OBJ_WRITE structures (inline mode)
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

// OBJ_READ structures (inline mode)
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

// Write flags
#define OBJ_WRITE_APPEND (1 << 0)

// OBJ_RENAME structures
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

// OBJ_DELETE structures
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

// OBJ_GET_INFO structures
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

// Initialize Storage Deck
void storage_deck_init(void);

// Main Storage Deck handler
int storage_deck_handler(Event *event);

// Synchronous I/O handlers (bypasses async path)
int handle_obj_read(Event *event);
int handle_obj_write(Event *event);

#endif // STORAGE_DECK_H
