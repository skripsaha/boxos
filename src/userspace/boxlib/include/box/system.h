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

/* Real per-cabin system snapshot — populated by `system.info` kernel op.
 *
 * Each numeric field is sized to its natural range (uptime in nanoseconds
 * fits 64 bits for ~584 years; physical memory amounts up to 16 EiB fit too).
 * No legacy 32-bit memory cap, no fake values. */
typedef struct {
    char     version[32];        /* "BoxOS X.Y", NUL-padded         */
    uint64_t uptime_ns;          /* nanosecond-precision uptime     */
    uint64_t total_memory;       /* total physical RAM (bytes)      */
    uint64_t used_memory;        /* in-use RAM (bytes)              */
    uint64_t free_memory;        /* free RAM (bytes)                */
    uint64_t tsc_freq_khz;       /* calibrated TSC freq, 0 = N/A    */
    uint32_t cpu_total;          /* total cores detected            */
    uint32_t cpu_k_cores;        /* K-Cores (kernel/IO)             */
    uint32_t cpu_app_cores;      /* App-Cores (userspace)           */
    uint32_t process_count;      /* live processes                  */
    uint32_t pit_freq_hz;        /* configured PIT frequency        */
    uint8_t  multicore_active;   /* 1 if AMP currently active       */
    uint8_t  has_invariant_tsc;  /* 1 if TSC stable across cores    */
    uint8_t  has_waitpkg;        /* 1 if UMWAIT available           */
    uint8_t  reserved;
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
