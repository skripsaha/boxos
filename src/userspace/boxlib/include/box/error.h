#ifndef BOX_ERROR_H
#define BOX_ERROR_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"

typedef uint32_t error_t;

#define OK                      0

/* ── BOX_ERROR_LIST — the single source of truth for every BoxOS error code ──
 * One row per code, mirroring the kernel enum (src/kernel/core/error/error.h)
 * and its strings (error.c ErrorString) — the same numbers, dense inside each
 * group until the first release:
 *
 *     X(SUFFIX, enumerator, value, "message", "category")
 *
 * EVERY downstream artifact expands from THIS list — the boxlib ERR_* code
 * constants below, the C++ box::errc enumerators (box/cxx/error.h), and the
 * box:: message + category tables (src/runtime/box_error.cpp). Add a code here
 * and all three layers follow; there is no second hand-list to drift against.
 * (The C preprocessor cannot emit a #define from a macro, so the codes are an
 * anonymous enum — exactly like the kernel — not #defines; nothing in the tree
 * keys off ERR_* being a macro, so this is transparent to every caller.) */
#define BOX_ERROR_LIST(X) \
    /* General (1-17) */ \
    X(UNKNOWN,                       unknown,                       1,    "unknown error",                 "core") \
    X(NOT_IMPLEMENTED,               not_implemented,               2,    "not implemented",               "core") \
    X(INVALID_ARGUMENT,              invalid_argument,              3,    "invalid argument",              "core") \
    X(NULL_POINTER,                  null_pointer,                  4,    "null pointer",                  "core") \
    X(OUT_OF_RANGE,                  out_of_range,                  5,    "out of range",                  "core") \
    X(BUFFER_TOO_SMALL,              buffer_too_small,              6,    "buffer too small",              "core") \
    X(TIMEOUT,                       timeout,                       7,    "timed out",                     "core") \
    X(BUSY,                          busy,                          8,    "busy",                          "core") \
    X(WOULD_BLOCK,                   would_block,                   9,    "operation would block",         "core") \
    X(CORRUPTED,                     corrupted,                     10,   "corrupted",                     "core") \
    X(INTERNAL,                      internal,                      11,   "internal error",                "core") \
    X(NOT_INITIALIZED,               not_initialized,               12,   "not initialized",               "core") \
    X(ALREADY_INITIALIZED,           already_initialized,           13,   "already initialized",           "core") \
    X(INVALID_STATE,                 invalid_state,                 14,   "invalid state",                 "core") \
    X(UNSUPPORTED,                   unsupported,                   15,   "unsupported operation",         "core") \
    X(CHECKSUM,                      checksum,                      16,   "checksum failed",               "core") \
    X(QUOTA_EXCEEDED,                quota_exceeded,                17,   "quota exceeded",                "core") \
    /* Memory (100-108) */ \
    X(NO_MEMORY,                     no_memory,                     100,  "out of memory",                 "memory") \
    X(INVALID_ADDRESS,               invalid_address,               101,  "invalid address",               "memory") \
    X(ALREADY_MAPPED,                already_mapped,                102,  "already mapped",                "memory") \
    X(NOT_MAPPED,                    not_mapped,                    103,  "not mapped",                    "memory") \
    X(PERMISSION_DENIED_MEM,         permission_denied_mem,         104,  "memory permission denied",      "memory") \
    X(HEAP_EXHAUSTED,                heap_exhausted,                105,  "heap exhausted",                "memory") \
    X(INVALID_BUFFER_ID,             invalid_buffer_id,             106,  "invalid buffer id",             "memory") \
    X(MAPPING_FAILED,                mapping_failed,                107,  "memory mapping failed",         "memory") \
    X(PCID_EXHAUSTED,                pcid_exhausted,                108,  "pcid pool exhausted",           "memory") \
    /* I/O (200-206) */ \
    X(IO,                            io,                            200,  "i/o error",                     "io") \
    X(READ_FAILED,                   read_failed,                   201,  "read failed",                   "io") \
    X(WRITE_FAILED,                  write_failed,                  202,  "write failed",                  "io") \
    X(DEVICE_NOT_READY,              device_not_ready,              203,  "device not ready",              "io") \
    X(STREAM_CLOSED,                 stream_closed,                 204,  "stream closed",                 "io") \
    X(BAD_SECTOR,                    bad_sector,                    205,  "bad sector",                    "io") \
    X(IO_CANCELLED,                  io_cancelled,                  206,  "i/o cancelled",                 "io") \
    /* Filesystem (300-304) */ \
    X(FILE_NOT_FOUND,                file_not_found,                300,  "file not found",                "storage") \
    X(OBJECT_NOT_FOUND,              object_not_found,              301,  "object not found",              "storage") \
    X(TAG_NOT_FOUND,                 tag_not_found,                 302,  "tag not found",                 "storage") \
    X(ALREADY_EXISTS,                already_exists,                303,  "already exists",                "storage") \
    X(METADATA_CORRUPTED,            metadata_corrupted,            304,  "metadata corrupted",            "storage") \
    /* Process (400-411) */ \
    X(PROCESS_NOT_FOUND,             process_not_found,             400,  "process not found",             "process") \
    X(INVALID_PID,                   invalid_pid,                   401,  "invalid pid",                   "process") \
    X(PROCESS_LIMIT_EXCEEDED,        process_limit_exceeded,        402,  "process limit exceeded",        "process") \
    X(PROCESS_TERMINATED,            process_terminated,            403,  "process terminated",            "process") \
    X(PROCESS_BLOCKED,               process_blocked,               404,  "process blocked",               "process") \
    X(INVALID_ELF,                   invalid_elf,                   405,  "invalid elf",                   "process") \
    X(BINARY_TOO_LARGE,              binary_too_large,              406,  "binary too large",              "process") \
    X(SPAWN_FAILED,                  spawn_failed,                  407,  "spawn failed",                  "process") \
    X(PROCESS_DESTROYING,            process_destroying,            408,  "process is being destroyed",    "process") \
    X(TAG_OVERFLOW_FAILED,           tag_overflow_failed,           409,  "tag overflow allocation failed","process") \
    X(PROCESS_KILLED,                process_killed,                410,  "process killed",                "process") \
    X(PROCESS_CRASHED,               process_crashed,               411,  "process crashed",               "process") \
    /* Security (500-502) */ \
    X(ACCESS_DENIED,                 access_denied,                 500,  "access denied",                 "security") \
    X(PERMISSION_DENIED,             permission_denied,             501,  "permission denied",             "security") \
    X(INVALID_OPERATION,             invalid_operation,             502,  "invalid operation",             "security") \
    /* Hardware (600) */ \
    X(CPU_ERROR,                     cpu_error,                     600,  "cpu error",                     "hardware") \
    /* TagFS core (850-863) */ \
    X(TAGFS_NOT_INITIALIZED,         tagfs_not_initialized,         850,  "tagfs not initialized",         "tagfs") \
    X(TAGFS_CORRUPTED,               tagfs_corrupted,               851,  "tagfs corrupted",               "tagfs") \
    X(TAGFS_NO_SPACE,                tagfs_no_space,                852,  "tagfs no space",                "tagfs") \
    X(TAGFS_INVALID_HANDLE,          tagfs_invalid_handle,          853,  "tagfs invalid handle",          "tagfs") \
    X(TAGFS_READ_ONLY,               tagfs_read_only,               854,  "tagfs read only",               "tagfs") \
    X(TAGFS_METADATA_ERROR,          tagfs_metadata_error,          855,  "tagfs metadata error",          "tagfs") \
    X(TAGFS_REGISTRY_FULL,           tagfs_registry_full,           856,  "tagfs registry full",           "tagfs") \
    X(TAGFS_TAG_NOT_FOUND,           tagfs_tag_not_found,           857,  "tagfs tag not found",           "tagfs") \
    X(TAGFS_INVALID_TAG,             tagfs_invalid_tag,             858,  "tagfs invalid tag",             "tagfs") \
    X(TAGFS_EXTENT_ERROR,            tagfs_extent_error,            859,  "tagfs extent error",            "tagfs") \
    X(TAGFS_TAG_TOO_LONG,            tagfs_tag_too_long,            860,  "tagfs tag too long",            "tagfs") \
    X(TAGFS_BEYOND_SIZE,             tagfs_beyond_size,             861,  "tagfs write past the size",     "tagfs") \
    X(TAGFS_RECORD_FULL,             tagfs_record_full,             862,  "tagfs file record full",        "tagfs") \
    X(TAGFS_FID_EXHAUSTED,           tagfs_fid_exhausted,           863,  "tagfs file ids exhausted",      "tagfs") \
    /* Pocket / IPC (900-904) */ \
    X(POCKET_RING_FULL,              pocket_ring_full,              900,  "pocket ring full",              "ipc") \
    X(INVALID_POCKET,                invalid_pocket,                901,  "invalid pocket",                "ipc") \
    X(INVALID_OPCODE,                invalid_opcode,                902,  "invalid opcode",                "ipc") \
    X(KCORE_QUEUE_FULL,              kcore_queue_full,              903,  "k-core queue full",             "ipc") \
    X(KCORE_SUBMIT_FAILED,           kcore_submit_failed,           904,  "k-core submit failed",          "ipc") \
    /* Routing (940-942) */ \
    X(ROUTE_NO_SUBSCRIBERS,          route_no_subscribers,          940,  "no subscribers for route",      "routing") \
    X(ROUTE_SELF,                    route_self,                    941,  "cannot route to self",          "routing") \
    X(ROUTE_AMBIGUOUS,               route_ambiguous,               942,  "more than one carries the route tag", "routing") \
    /* Scheduler (960-961) */ \
    X(RUNQUEUE_FULL,                 runqueue_full,                 960,  "runqueue full",                 "scheduler") \
    X(HOME_CORE_INVALID,             home_core_invalid,             961,  "invalid home core",             "scheduler") \
    /* Boot (970) */ \
    X(E820_FAILED,                   e820_failed,                   970,  "e820 memory detection failed",  "boot") \
    /* TagFS module — a write the mount found never landed (1000) */ \
    X(TAGFS_NOT_LANDED,              tagfs_not_landed,              1000, "tagfs write did not land",      "tagfs") \
    /* TagFS module — Braid RAID (1050-1053) */ \
    X(BRAID_DISK_OFFLINE,            braid_disk_offline,            1050, "braid disk offline",            "braid") \
    X(BRAID_HEAL_FAILED,             braid_heal_failed,             1051, "braid heal failed",             "braid") \
    X(BRAID_INSUFFICIENT_DISKS,      braid_insufficient_disks,      1052, "braid insufficient disks",      "braid") \
    X(BRAID_MODE_INVALID,            braid_mode_invalid,            1053, "braid invalid mode",            "braid") \
    /* Strand (1100) */ \
    X(ADDR_VALUE_MISMATCH,           addr_value_mismatch,           1100, "park value mismatch",           "strand")

/* The boxlib code constants — emitted from the list above (kernel parity). */
enum {
#define BOX_ERROR_DEFINE(SUFFIX, name, value, msg, cat) ERR_##SUFFIX = (value),
    BOX_ERROR_LIST(BOX_ERROR_DEFINE)
#undef BOX_ERROR_DEFINE
};

/* Convenience aliases (boxlib-only; resolve to codes defined above). */
#define ERR_INVALID_ARGS        ERR_INVALID_ARGUMENT
#define ERR_RESULT_INVALID      ERR_CORRUPTED

#define IS_ERROR(err)   ((err) != OK)
#define IS_SUCCESS(err) ((err) == OK)

/* ── error-cause preservation (Ф23) ──────────────────────────────────────
 * BoxOS keeps a failed call's real error_t in its RETURN VALUE — never in a
 * system-wide errno register (that Unix idiom is deliberately absent). A
 * boxlib syscall stub gets the cause back from MfCall1 in three dialects: 0
 * on success, a POSITIVE error_t for a kernel-side error, and a NEGATIVE
 * -ERR_* for a builder/submit (transport) failure. box_fail folds the two
 * failure dialects into one negative form, so a stub reports failure as a
 * value < 0 whose magnitude is the real error_t (the boxlib stubs are
 * converted to this rule in Ф23b). The caller — plain C, or the C++
 * box::result bridge — recovers the cause with box_errno_of.
 *
 * One rule across all of boxlib: a return  < 0  is failure with
 * error_t == box_errno_of(ret); a return >= 0 is success (a payload or 0). */
static inline int box_fail(int rc)
{
    /* 0 → 0 (caller maps to its own success); kernel +error_t → negate;
     * transport -ERR_* → keep. Result is <= 0, and 0 only when rc == 0. */
    return rc == 0 ? 0 : (rc > 0 ? -rc : rc);
}

static inline error_t box_errno_of(int ret)
{
    /* Recover the error_t a failing stub returned. ret >= 0 carries no
     * cause. Widen before negating so INT_MIN cannot overflow. */
    return ret >= 0 ? OK : (error_t)(-(long)ret);
}

#ifdef __cplusplus
}
#endif

#endif // BOX_ERROR_H
