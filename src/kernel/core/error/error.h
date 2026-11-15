#ifndef BOXOS_ERROR_H
#define BOXOS_ERROR_H

#include "ktypes.h"

typedef enum {
    OK = 0,

    /* Each group keeps its first number, and the codes inside it are dense
     * until the first release: a code nothing produces any more goes, and
     * the codes after it in its group move down. boxlib's BOX_ERROR_LIST
     * (box/error.h) carries the same numbers (tools/check_error_parity.sh). */

    // General Errors (1-99)
    ERR_UNKNOWN = 1,
    ERR_NOT_IMPLEMENTED = 2,
    ERR_INVALID_ARGUMENT = 3,
    ERR_NULL_POINTER = 4,
    ERR_OUT_OF_RANGE = 5,
    ERR_BUFFER_TOO_SMALL = 6,
    ERR_TIMEOUT = 7,
    ERR_BUSY = 8,
    ERR_WOULD_BLOCK = 9,
    ERR_CORRUPTED = 10,
    ERR_INTERNAL = 11,
    ERR_NOT_INITIALIZED = 12,
    ERR_ALREADY_INITIALIZED = 13,
    ERR_INVALID_STATE = 14,
    ERR_UNSUPPORTED = 15,
    ERR_CHECKSUM = 16,
    /* Per-resource quota limit reached (e.g. per-process TME-MK
     * KeyID quota exhausted). Distinct from ERR_NO_MEMORY (global
     * memory pressure) and ERR_BUSY (transient) — the caller can
     * retry only after releasing existing resources. */
    ERR_QUOTA_EXCEEDED = 17,

    // Memory Errors (100-199)
    ERR_NO_MEMORY = 100,
    ERR_INVALID_ADDRESS = 101,
    ERR_ALREADY_MAPPED = 102,
    ERR_NOT_MAPPED = 103,
    ERR_PERMISSION_DENIED_MEM = 104,
    ERR_HEAP_EXHAUSTED = 105,
    ERR_INVALID_BUFFER_ID = 106,
    ERR_MAPPING_FAILED = 107,
    ERR_PCID_EXHAUSTED = 108,

    // I/O Errors (200-299)
    ERR_IO = 200,
    ERR_READ_FAILED = 201,
    ERR_WRITE_FAILED = 202,
    ERR_DEVICE_NOT_READY = 203,
    ERR_STREAM_CLOSED = 204,
    ERR_BAD_SECTOR = 205,
    ERR_IO_CANCELLED = 206,

    // Filesystem Errors (300-399)
    ERR_FILE_NOT_FOUND = 300,
    ERR_OBJECT_NOT_FOUND = 301,
    ERR_TAG_NOT_FOUND = 302,
    ERR_ALREADY_EXISTS = 303,
    ERR_METADATA_CORRUPTED = 304,

    // Process Errors (400-499)
    ERR_PROCESS_NOT_FOUND = 400,
    ERR_INVALID_PID = 401,
    ERR_PROCESS_LIMIT_EXCEEDED = 402,
    ERR_PROCESS_TERMINATED = 403,
    ERR_PROCESS_BLOCKED = 404,
    ERR_INVALID_ELF = 405,
    ERR_BINARY_TOO_LARGE = 406,
    ERR_SPAWN_FAILED = 407,
    ERR_PROCESS_DESTROYING = 408,
    ERR_TAG_OVERFLOW_FAILED = 409,
    ERR_PROCESS_KILLED = 410,
    ERR_PROCESS_CRASHED = 411,

    // Security Errors (500-599)
    ERR_ACCESS_DENIED = 500,
    ERR_PERMISSION_DENIED = 501,
    ERR_INVALID_OPERATION = 502,

    // Hardware Errors (600-699)
    ERR_CPU_ERROR = 600,

    // Network Errors (700-799) - reserved

    // Pocket System Errors (900-999)
    ERR_POCKET_RING_FULL = 900,
    ERR_INVALID_POCKET = 901,
    ERR_INVALID_OPCODE = 902,
    ERR_KCORE_QUEUE_FULL = 903,
    ERR_KCORE_SUBMIT_FAILED = 904,

    // Routing/IPC Errors (940-949)
    ERR_ROUTE_NO_SUBSCRIBERS = 940,
    ERR_ROUTE_SELF           = 941,
    ERR_ROUTE_AMBIGUOUS      = 942,

    // Scheduler Errors (960-969)
    ERR_RUNQUEUE_FULL = 960,
    ERR_HOME_CORE_INVALID = 961,

    // Boot Errors (970-979)
    ERR_E820_FAILED = 970,

    // TagFS Core Errors (850-869)
    ERR_TAGFS_NOT_INITIALIZED = 850,
    ERR_TAGFS_CORRUPTED = 851,
    ERR_TAGFS_NO_SPACE = 852,
    ERR_TAGFS_INVALID_HANDLE = 853,
    ERR_TAGFS_READ_ONLY = 854,
    ERR_TAGFS_METADATA_ERROR = 855,
    ERR_TAGFS_REGISTRY_FULL = 856,
    ERR_TAGFS_TAG_NOT_FOUND = 857,
    ERR_TAGFS_INVALID_TAG = 858,
    ERR_TAGFS_EXTENT_ERROR = 859,
    /* A key past its u8 width, or a tag wider than the book carries one (tag_spelling.h). */
    ERR_TAGFS_TAG_TOO_LONG = 860,
    /* A write asked to begin past the file's size: a file grows at its end, or by span. */
    ERR_TAGFS_BEYOND_SIZE = 861,
    /* A defrag found no run of the file's extents it could gather into one
     * (plot_gather.c). */
    ERR_TAGFS_RECORD_FULL = 862,
    /* Every file id a program can name has been issued — past INT64_MAX, read
     * back as a signed 64-bit number — and a volume never issues one twice
     * (ink_book.c). */
    ERR_TAGFS_FID_EXHAUSTED = 863,

    // TagFS Module Errors (1000-1099)
    /* A write the book's mount found never reached the medium: the stitch that
     * named its data landed and the data did not (spec_v2 §3.9) — the file is
     * rolled back by its splice, or never born. */
    ERR_TAGFS_NOT_LANDED = 1000,

    // Braid RAID
    ERR_BRAID_DISK_OFFLINE = 1050,
    ERR_BRAID_HEAL_FAILED = 1051,
    ERR_BRAID_INSUFFICIENT_DISKS = 1052,
    ERR_BRAID_MODE_INVALID = 1053,

    /* Strand addr_park: *addr != expected at park time (no park performed). */
    ERR_ADDR_VALUE_MISMATCH = 1100,

    ERR_MAX = 1100
} error_t;

#define IS_ERROR(err) ((err) != OK)
#define IS_SUCCESS(err) ((err) == OK)

#define PROPAGATE(expr) do { \
    error_t _err = (expr); \
    if (IS_ERROR(_err)) { \
        return _err; \
    } \
} while (0)

const char* ErrorString(error_t err);

#endif // BOXOS_ERROR_H
