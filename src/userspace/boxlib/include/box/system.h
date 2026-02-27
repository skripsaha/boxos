#ifndef BOX_SYSTEM_H
#define BOX_SYSTEM_H

#include "types.h"
#include "error.h"

#define BOX_SYSTEM_DECK_ID          BOX_DECK_SYSTEM

#define BOX_SYSTEM_PROC_INFO        0x03
#define BOX_SYSTEM_EXIT             0x02
#define BOX_SYSTEM_BUFFER_ALLOC     0x10
#define BOX_SYSTEM_BUFFER_FREE      0x11
#define BOX_SYSTEM_DEFRAG_FILE      0x18
#define BOX_SYSTEM_FRAG_SCORE       0x19
#define BOX_SYSTEM_TAG_ADD          0x20
#define BOX_SYSTEM_TAG_REMOVE       0x21
#define BOX_SYSTEM_TAG_CHECK        0x22

#define BOX_BUFFER_SIZE_256         0
#define BOX_BUFFER_SIZE_512         1
#define BOX_BUFFER_SIZE_1K          2
#define BOX_BUFFER_SIZE_2K          3
#define BOX_BUFFER_SIZE_4K          4

#define BOX_PROC_STATE_CREATED      0
#define BOX_PROC_STATE_READY        1
#define BOX_PROC_STATE_RUNNING      2
#define BOX_PROC_STATE_WAITING      3
#define BOX_PROC_STATE_TERMINATED   4

typedef struct {
    uint16_t pid;
    uint8_t state;
    uint8_t priority;
    uint32_t memory_usage;
} proc_info_t;

typedef struct {
    char version[32];
    uint32_t uptime_seconds;
    uint32_t total_memory;
    uint32_t used_memory;
} system_info_t;

// Process
int proc_info(uint16_t pid, proc_info_t* info);
void exit(uint32_t exit_code);

// Buffers
int buffer_alloc(uint8_t size_class, uint16_t* out_buffer_id, uint32_t* out_address);
int buffer_free(uint16_t buffer_id);

// Process Tags
int proc_tag_add(const char* tag);
int proc_tag_remove(const char* tag);
int proc_tag_check(const char* tag, bool* has_tag);

// Power
int reboot(void);
int shutdown(void);

// Info
int sysinfo(system_info_t* info);

// Filesystem
int defrag(uint32_t file_id, uint32_t target_block);
int fragmentation(void);

// Scheduling
void yield(void);

#endif // BOX_SYSTEM_H
