#ifndef SYSTEM_DECK_PROCESS_H
#define SYSTEM_DECK_PROCESS_H

#include "events.h"
#include "process.h"
#include "kernel_config.h"

#define SYSTEM_ERR_SUCCESS           0x0000
#define SYSTEM_ERR_INVALID_ARGS      0x0001
#define SYSTEM_ERR_NO_MEMORY         0x0002
#define SYSTEM_ERR_PROCESS_LIMIT     0x0003
#define SYSTEM_ERR_PROCESS_NOT_FOUND 0x0004
#define SYSTEM_ERR_LOAD_FAILED       0x0005
#define SYSTEM_ERR_CABIN_FAILED      0x0006
#define SYSTEM_ERR_SIZE_LIMIT        0x0007
#define SYSTEM_ERR_BUFFER_NOT_FOUND  0x0008
#define SYSTEM_ERR_BUFFER_LIMIT      0x0009
#define SYSTEM_ERR_TAG_EXISTS        0x000A
#define SYSTEM_ERR_TAG_NOT_FOUND     0x000B
#define SYSTEM_ERR_TAG_FULL          0x000C
#define SYSTEM_ERR_NOT_IMPLEMENTED   0x00FF

#define PROC_SPAWN_MAX_BINARY_SIZE   CONFIG_PROC_MAX_BINARY_SIZE
#define PROC_INFO_TAGS_SIZE          64

#define BUF_MAX_SIZE                 CONFIG_PROC_MAX_BUFFER_SIZE
#define BUF_MAX_COUNT                64

// Process spawn limits
#define PROC_SPAWN_MAX_PHYS_ADDR     0x100000000ULL  // 4GB physical address limit for 32-bit compatibility

typedef struct __packed {
    uint64_t binary_phys_addr;
    uint64_t binary_size;
    char tags[128];
} proc_spawn_event_t;

typedef struct __packed {
    uint32_t new_pid;
    uint32_t reserved;
} proc_spawn_response_t;

typedef struct __packed {
    uint32_t target_pid;
    uint32_t reserved;
} proc_kill_event_t;

typedef struct __packed {
    uint32_t killed_pid;
    uint32_t reserved;
} proc_kill_response_t;

typedef struct __packed {
    uint32_t target_pid;
    uint32_t reserved;
} proc_info_event_t;

typedef struct __packed {
    uint32_t pid;
    uint32_t state;
    int32_t score;
    uint32_t reserved1;
    uint64_t notify_page_phys;
    uint64_t result_page_phys;
    uint64_t code_start;
    uint64_t code_size;
    bool result_there;
    uint8_t reserved2[7];
    char tags[PROC_INFO_TAGS_SIZE];
} proc_info_response_t;

typedef struct __packed {
    uint64_t size;
    uint32_t flags;
    uint8_t reserved[180];
} buf_alloc_event_t;

typedef struct __packed {
    uint64_t buffer_handle;
    uint64_t phys_addr;
    uint64_t actual_size;
    uint32_t error_code;
    uint8_t reserved[164];
} buf_alloc_response_t;

typedef struct __packed {
    uint64_t buffer_handle;
    uint8_t reserved[184];
} buf_free_event_t;

typedef struct __packed {
    uint32_t error_code;
    uint8_t reserved[188];
} buf_free_response_t;

typedef struct __packed {
    uint64_t buffer_handle;
    uint64_t new_size;
    uint8_t reserved[176];
} buf_resize_event_t;

typedef struct __packed {
    uint64_t buffer_handle;
    uint64_t actual_size;
    uint32_t error_code;
    uint8_t reserved[168];
} buf_resize_response_t;

typedef struct __packed {
    uint32_t target_pid;
    char tag[64];
    uint8_t reserved[124];
} tag_modify_event_t;

typedef struct __packed {
    uint32_t error_code;
    char message[128];
    uint8_t reserved[60];
} tag_modify_response_t;

typedef struct __packed {
    uint32_t target_pid;
    char tag[64];
    uint8_t reserved[124];
} tag_check_event_t;

typedef struct __packed {
    bool has_tag;
    uint8_t reserved1[3];
    uint32_t error_code;
    uint8_t reserved2[184];
} tag_check_response_t;

int system_deck_proc_spawn(Event* event);
int system_deck_proc_kill(Event* event);
int system_deck_proc_info(Event* event);

int system_deck_buf_alloc(Event* event);
int system_deck_buf_free(Event* event);
int system_deck_buf_resize(Event* event);

int system_deck_tag_add(Event* event);
int system_deck_tag_remove(Event* event);
int system_deck_tag_check(Event* event);

// CRITICAL-3: Buffer cleanup for process destruction
// Call this when a process is destroyed to free all its buffers
// This prevents memory leaks when processes terminate
// NOTE: Process management code should call this during process_destroy()
void system_deck_cleanup_process_buffers(uint32_t pid);

#endif // SYSTEM_DECK_PROCESS_H
