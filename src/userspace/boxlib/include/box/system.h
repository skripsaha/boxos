#ifndef SYSTEM_H
#define SYSTEM_H

#include "box/types.h"
#include "box/error.h"

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

int proc_tag_add(const char* tag);
int proc_tag_remove(const char* tag);
int proc_tag_check(const char* tag, bool* has_tag);

int reboot(void);
int shutdown(void);
int sysinfo(system_info_t* info);

int defrag(uint32_t file_id, uint32_t target_block);
int fragmentation(void);
int perf_dump(void);

void yield(void);

#endif // SYSTEM_H
