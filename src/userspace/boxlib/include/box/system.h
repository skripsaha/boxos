#ifndef SYSTEM_H
#define SYSTEM_H

#include "types.h"
#include "error.h"

#define SYSTEM_DECK_ID          DECK_SYSTEM

#define SYSTEM_PROC_INFO        0x03
#define SYSTEM_PROC_EXEC        0x06
#define SYSTEM_EXIT             0x02
#define SYSTEM_BUFFER_ALLOC     0x10
#define SYSTEM_BUFFER_FREE      0x11
#define SYSTEM_DEFRAG_FILE      0x18
#define SYSTEM_FRAG_SCORE       0x19
#define SYSTEM_TAG_ADD          0x20
#define SYSTEM_TAG_REMOVE       0x21
#define SYSTEM_TAG_CHECK        0x22

#define BUFFER_SIZE_256         0
#define BUFFER_SIZE_512         1
#define BUFFER_SIZE_1K          2
#define BUFFER_SIZE_2K          3
#define BUFFER_SIZE_4K          4

#define PROC_STATE_CREATED      0
#define PROC_STATE_READY        1
#define PROC_STATE_RUNNING      2
#define PROC_STATE_WAITING      3
#define PROC_STATE_TERMINATED   4

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

int proc_info(uint16_t pid, proc_info_t* info);
void exit(uint32_t exit_code);
int proc_exec(const char* filename);

int buffer_alloc(uint8_t size_class, uint16_t* out_buffer_id, uint32_t* out_address);
int buffer_free(uint16_t buffer_id);

int proc_tag_add(const char* tag);
int proc_tag_remove(const char* tag);
int proc_tag_check(const char* tag, bool* has_tag);

int reboot(void);
int shutdown(void);
int sysinfo(system_info_t* info);

int defrag(uint32_t file_id, uint32_t target_block);
int fragmentation(void);

void yield(void);

#endif // SYSTEM_H
