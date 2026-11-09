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
//
// The program: program_file is the volume's id of the file it was started
// from (0 when the spawner handed the image over as bytes); image_base what
// was added to every place of its image (0 for an image where it was linked);
// parts_addr where the table of its parts in memory is (box_loaded.h), 0 for
// a program that is no box; raised how many times the system raised it again
// after it fell (rise.h), 0 at its first start.

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
    uint32_t raised;             // times the system raised it again after a fall
    uint64_t program_file;       // the volume's id of the program's file, 0 = none
    uint64_t image_base;         // added to every place of the image
    uint64_t parts_addr;         // user VA of the table of its parts in memory, 0 = none
} CabinInfo;

_Static_assert(sizeof(CabinInfo) == 88, "CabinInfo header must be 88 bytes");

/* Luggage that fits the rest of the page lives right after the header. */
#define CABIN_LUGGAGE_INLINE_OFFSET  ((uint32_t)sizeof(CabinInfo))

static inline bool cabin_info_valid(const CabinInfo* ci)
{
    return ci && ci->magic == CABIN_INFO_MAGIC;
}

#endif // CABIN_INFO_H
