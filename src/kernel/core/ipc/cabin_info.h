#ifndef CABIN_INFO_H
#define CABIN_INFO_H

#include "ktypes.h"
#include "boxos_magic.h"

// CabinInfo: read-only process metadata at fixed address 0x1000.
// Kernel fills this at process creation. Userspace reads pid, heap_base, etc.
//
// The Luggage — what the spawner said to this cabin at boarding, the command
// line as the person typed it — rides in the same page: luggage_addr is where
// the bytes are (inside this page, right after the header, when they fit; in
// the cabin's buffer heap when they do not) and luggage_length how many. Both
// zero for a cabin that was given nothing (autostart, a bare proc_exec).

typedef struct __packed {
    uint32_t magic;              // CABIN_INFO_MAGIC ("CABN")
    uint32_t pid;
    uint32_t spawner_pid;
    uint32_t generation;         // pid_generation at creation: (pid, generation) is who this is
    uint64_t heap_base;          // ASLR randomized heap start
    uint64_t heap_max_size;      // max heap size
    uint64_t buf_heap_base;      // ASLR randomized buffer heap start
    uint64_t stack_top;          // ASLR randomized stack top
    uint64_t luggage_addr;       // user VA of the luggage bytes, 0 = none
    uint32_t luggage_length;     // byte count, not NUL-terminated on the wire
    uint32_t luggage_reserved;
} CabinInfo;

_Static_assert(sizeof(CabinInfo) == 64, "CabinInfo header must be 64 bytes");

/* Luggage that fits the rest of the page lives right after the header. */
#define CABIN_LUGGAGE_INLINE_OFFSET  ((uint32_t)sizeof(CabinInfo))

static inline bool cabin_info_valid(const CabinInfo* ci)
{
    return ci && ci->magic == CABIN_INFO_MAGIC;
}

#endif // CABIN_INFO_H
