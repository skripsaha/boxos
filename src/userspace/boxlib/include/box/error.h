#ifndef BOX_ERROR_H
#define BOX_ERROR_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"

typedef uint32_t error_t;

#define OK                      0

#define BOX_ERROR_LIST(X) \
     \
    X(UNKNOWN,                       unknown,                       1,    "unknown error",                 "core") \
    X(NOT_IMPLEMENTED,               not_implemented,               2,    "not implemented",               "core") \
    X(INVALID_ARGUMENT,              invalid_argument,              3,    "invalid argument",              "core") \
    X(NULL_POINTER,                  null_pointer,                  4,    "null pointer",                  "core") \
    X(OUT_OF_RANGE,                  out_of_range,                  5,    "out of range",                  "core") \
    X(BUFFER_TOO_SMALL,              buffer_too_small,              6,    "buffer too small",              "core") \
    X(TIMEOUT,                       timeout,                       7,    "timed out",                     "core") \
    X(BUSY,                          busy,                          8,    "busy",                          "core") \
    X(WOULD_BLOCK,                   would_block,                   9,    "operation would block",         "core") \
    X(ALIGNMENT,                     alignment,                     10,   "bad alignment",                 "core") \
    X(CORRUPTED,                     corrupted,                     11,   "corrupted",                     "core") \
    X(INTERNAL,                      internal,                      12,   "internal error",                "core") \
    X(NOT_INITIALIZED,               not_initialized,               13,   "not initialized",               "core") \
    X(ALREADY_INITIALIZED,           already_initialized,           14,   "already initialized",           "core") \
    X(INVALID_STATE,                 invalid_state,                 15,   "invalid state",                 "core") \
    X(RETRY,                         retry,                         16,   "retry operation",               "core") \
    X(UNSUPPORTED,                   unsupported,                   17,   "unsupported operation",         "core") \
    X(VERSION_MISMATCH,              version_mismatch,              18,   "version mismatch",              "core") \
    X(CHECKSUM,                      checksum,                      19,   "checksum failed",               "core") \
    X(QUOTA_EXCEEDED,                quota_exceeded,                20,   "quota exceeded",                "core") \
     \
    X(NO_MEMORY,                     no_memory,                     100,  "out of memory",                 "memory") \
    X(INVALID_ADDRESS,               invalid_address,               101,  "invalid address",               "memory") \
    X(PAGE_FAULT,                    page_fault,                    102,  "page fault",                    "memory") \
    X(ALREADY_MAPPED,                already_mapped,                103,  "already mapped",                "memory") \
    X(NOT_MAPPED,                    not_mapped,                    104,  "not mapped",                    "memory") \
    X(PERMISSION_DENIED_MEM,         permission_denied_mem,         105,  "memory permission denied",      "memory") \
    X(HEAP_EXHAUSTED,                heap_exhausted,                106,  "heap exhausted",                "memory") \
    X(STACK_OVERFLOW,                stack_overflow,                107,  "stack overflow",                "memory") \
    X(BUFFER_OVERFLOW,               buffer_overflow,               108,  "buffer overflow",               "memory") \
    X(INVALID_BUFFER_ID,             invalid_buffer_id,             109,  "invalid buffer id",             "memory") \
    X(BUFFER_IN_USE,                 buffer_in_use,                 110,  "buffer in use",                 "memory") \
    X(BUFFER_LIMIT_EXCEEDED,         buffer_limit_exceeded,         111,  "buffer limit exceeded",         "memory") \
    X(MAPPING_FAILED,                mapping_failed,                112,  "memory mapping failed",         "memory") \
    X(UNMAP_FAILED,                  unmap_failed,                  113,  "memory unmapping failed",       "memory") \
    X(TLB_FLUSH_FAILED,              tlb_flush_failed,              114,  "tlb flush failed",              "memory") \
    X(PHYS_ADDR_EXCEEDED,            phys_addr_exceeded,            115,  "physical address exceeded",     "memory") \
    X(PCID_EXHAUSTED,                pcid_exhausted,                116,  "pcid pool exhausted",           "memory") \
     \
    X(IO,                            io,                            200,  "i/o error",                     "io") \
    X(READ_FAILED,                   read_failed,                   201,  "read failed",                   "io") \
    X(WRITE_FAILED,                  write_failed,                  202,  "write failed",                  "io") \
    X(DEVICE_NOT_READY,              device_not_ready,              203,  "device not ready",              "io") \
    X(DEVICE_ERROR,                  device_error,                  204,  "device error",                  "io") \
    X(STREAM_CLOSED,                 stream_closed,                 205,  "stream closed",                 "io") \
    X(DISK_FULL,                     disk_full,                     206,  "disk full",                     "io") \
    X(BAD_SECTOR,                    bad_sector,                    207,  "bad sector",                    "io") \
    X(IO_PENDING,                    io_pending,                    208,  "i/o pending",                   "io") \
    X(IO_QUEUE_FULL,                 io_queue_full,                 209,  "i/o queue full",                "io") \
    X(IO_CANCELLED,                  io_cancelled,                  210,  "i/o cancelled",                 "io") \
    X(DMA_FAILED,                    dma_failed,                    211,  "dma failed",                    "io") \
    X(DMA_TIMEOUT,                   dma_timeout,                   212,  "dma timeout",                   "io") \
    X(SECTOR_READ_FAILED,            sector_read_failed,            213,  "sector read failed",            "io") \
    X(SECTOR_WRITE_FAILED,           sector_write_failed,           214,  "sector write failed",           "io") \
     \
    X(FILE_NOT_FOUND,                file_not_found,                300,  "file not found",                "storage") \
    X(OBJECT_NOT_FOUND,              object_not_found,              301,  "object not found",              "storage") \
    X(TAG_NOT_FOUND,                 tag_not_found,                 302,  "tag not found",                 "storage") \
    X(ALREADY_EXISTS,                already_exists,                303,  "already exists",                "storage") \
    X(INVALID_TAG,                   invalid_tag,                   304,  "invalid tag",                   "storage") \
    X(TAG_LIMIT_EXCEEDED,            tag_limit_exceeded,            305,  "tag limit exceeded",            "storage") \
    X(OBJECT_CORRUPTED,              object_corrupted,              306,  "object corrupted",              "storage") \
    X(JOURNAL_FULL,                  journal_full,                  307,  "journal full",                  "storage") \
    X(JOURNAL_CORRUPTED,             journal_corrupted,             308,  "journal corrupted",             "storage") \
    X(METADATA_CORRUPTED,            metadata_corrupted,            309,  "metadata corrupted",            "storage") \
    X(TAG_REGISTRY_FULL,             tag_registry_full,             310,  "tag registry full",             "storage") \
    X(FILE_TABLE_CORRUPT,            file_table_corrupt,            311,  "file table corrupt",            "storage") \
    X(METADATA_POOL_FULL,            metadata_pool_full,            312,  "metadata pool full",            "storage") \
    X(BLOCK_ALLOC_FAILED,            block_alloc_failed,            313,  "block allocation failed",       "storage") \
    X(BLOCK_FREE_FAILED,             block_free_failed,             314,  "block free failed",             "storage") \
    X(FILE_HANDLE_INVALID,           file_handle_invalid,           315,  "invalid file handle",           "storage") \
    X(FILE_HANDLE_IN_USE,            file_handle_in_use,            316,  "file handle in use",            "storage") \
    X(EXTENT_INVALID,                extent_invalid,                317,  "invalid extent",                "storage") \
    X(BITMAP_FULL,                   bitmap_full,                   318,  "bitmap full",                   "storage") \
    X(SUPERBLOCK_INVALID,            superblock_invalid,            319,  "invalid superblock",            "storage") \
    X(BOOT_HINTS_MISSING,            boot_hints_missing,            320,  "boot hints missing",            "storage") \
    X(SNAPSHOT_LIMIT,                snapshot_limit,                321,  "snapshot limit reached",        "storage") \
    X(SNAPSHOT_NOT_FOUND,            snapshot_not_found,            322,  "snapshot not found",            "storage") \
    X(DEDUP_FAILED,                  dedup_failed,                  323,  "deduplication failed",          "storage") \
    X(SELF_HEAL_FAILED,              self_heal_failed,              324,  "self-healing failed",           "storage") \
     \
    X(PROCESS_NOT_FOUND,             process_not_found,             400,  "process not found",             "process") \
    X(INVALID_PID,                   invalid_pid,                   401,  "invalid pid",                   "process") \
    X(PROCESS_LIMIT_EXCEEDED,        process_limit_exceeded,        402,  "process limit exceeded",        "process") \
    X(PROCESS_TERMINATED,            process_terminated,            403,  "process terminated",            "process") \
    X(PROCESS_BLOCKED,               process_blocked,               404,  "process blocked",               "process") \
    X(INVALID_ELF,                   invalid_elf,                   405,  "invalid elf",                   "process") \
    X(BINARY_TOO_LARGE,              binary_too_large,              406,  "binary too large",              "process") \
    X(SPAWN_FAILED,                  spawn_failed,                  407,  "spawn failed",                  "process") \
    X(PROCESS_RUNNING,               process_running,               408,  "process is running",            "process") \
    X(PROCESS_DESTROYING,            process_destroying,            409,  "process is being destroyed",    "process") \
    X(CABIN_CREATE_FAILED,           cabin_create_failed,           410,  "cabin creation failed",         "process") \
    X(PID_EXHAUSTED,                 pid_exhausted,                 411,  "pid pool exhausted",            "process") \
    X(TAG_OVERFLOW_FAILED,           tag_overflow_failed,           412,  "tag overflow allocation failed","process") \
    X(FPU_INIT_FAILED,               fpu_init_failed,               413,  "fpu initialization failed",     "process") \
    X(STACK_ALLOC_FAILED,            stack_alloc_failed,            414,  "stack allocation failed",       "process") \
    X(PROCESS_KILLED,                process_killed,                415,  "process killed",                "process") \
    X(PROCESS_CRASHED,               process_crashed,               416,  "process crashed",               "process") \
     \
    X(ACCESS_DENIED,                 access_denied,                 500,  "access denied",                 "security") \
    X(PERMISSION_DENIED,             permission_denied,             501,  "permission denied",             "security") \
    X(SECURITY_VIOLATION,            security_violation,            502,  "security violation",            "security") \
    X(TAG_MISMATCH,                  tag_mismatch,                  503,  "tag mismatch",                  "security") \
    X(INVALID_OPERATION,             invalid_operation,             504,  "invalid operation",             "security") \
    X(PRIVILEGE_REQUIRED,            privilege_required,            505,  "privilege required",            "security") \
    X(SANDBOX_VIOLATION,             sandbox_violation,             506,  "sandbox violation",             "security") \
     \
    X(HARDWARE,                      hardware,                      600,  "hardware error",                "hardware") \
    X(INVALID_DEVICE,                invalid_device,                601,  "invalid device",                "hardware") \
    X(DEVICE_BUSY,                   device_busy,                   602,  "device busy",                   "hardware") \
    X(KEYBOARD_BUFFER_FULL,          keyboard_buffer_full,          603,  "keyboard buffer full",          "hardware") \
    X(VGA_ERROR,                     vga_error,                     604,  "vga error",                     "hardware") \
    X(ATA_ERROR,                     ata_error,                     605,  "ata error",                     "hardware") \
    X(PCI_ERROR,                     pci_error,                     606,  "pci error",                     "hardware") \
    X(USB_ERROR,                     usb_error,                     607,  "usb error",                     "hardware") \
    X(XHCI_ERROR,                    xhci_error,                    608,  "xhci error",                    "hardware") \
    X(AHCI_ERROR,                    ahci_error,                    609,  "ahci error",                    "hardware") \
    X(TIMER_ERROR,                   timer_error,                   610,  "timer error",                   "hardware") \
    X(INTERRUPT_ERROR,               interrupt_error,               611,  "interrupt error",               "hardware") \
    X(CPU_ERROR,                     cpu_error,                     612,  "cpu error",                     "hardware") \
     \
    X(ACPI_NOT_FOUND,                acpi_not_found,                800,  "acpi tables not found",         "acpi") \
    X(ACPI_INVALID_TABLE,            acpi_invalid_table,            801,  "invalid acpi table",            "acpi") \
    X(ACPI_CHECKSUM_FAILED,          acpi_checksum_failed,          802,  "acpi checksum failed",          "acpi") \
    X(ACPI_PARSE_ERROR,              acpi_parse_error,              803,  "acpi parse error",              "acpi") \
    X(ACPI_MADT_NOT_FOUND,           acpi_madt_not_found,           804,  "acpi madt not found",           "acpi") \
    X(ACPI_FADT_NOT_FOUND,           acpi_fadt_not_found,           805,  "acpi fadt not found",           "acpi") \
     \
    X(TAGFS_NOT_INITIALIZED,         tagfs_not_initialized,         850,  "tagfs not initialized",         "tagfs") \
    X(TAGFS_CORRUPTED,               tagfs_corrupted,               851,  "tagfs corrupted",               "tagfs") \
    X(TAGFS_NO_SPACE,                tagfs_no_space,                852,  "tagfs no space",                "tagfs") \
    X(TAGFS_FILE_NOT_FOUND,          tagfs_file_not_found,          853,  "tagfs file not found",          "tagfs") \
    X(TAGFS_FILE_EXISTS,             tagfs_file_exists,             854,  "tagfs file exists",             "tagfs") \
    X(TAGFS_INVALID_HANDLE,          tagfs_invalid_handle,          855,  "tagfs invalid handle",          "tagfs") \
    X(TAGFS_READ_ONLY,               tagfs_read_only,               856,  "tagfs read only",               "tagfs") \
    X(TAGFS_QUOTA_EXCEEDED,          tagfs_quota_exceeded,          857,  "tagfs quota exceeded",          "tagfs") \
    X(TAGFS_METADATA_ERROR,          tagfs_metadata_error,          858,  "tagfs metadata error",          "tagfs") \
    X(TAGFS_BITMAP_FULL,             tagfs_bitmap_full,             859,  "tagfs bitmap full",             "tagfs") \
    X(TAGFS_REGISTRY_FULL,           tagfs_registry_full,           860,  "tagfs registry full",           "tagfs") \
    X(TAGFS_TAG_NOT_FOUND,           tagfs_tag_not_found,           861,  "tagfs tag not found",           "tagfs") \
    X(TAGFS_TAG_EXISTS,              tagfs_tag_exists,              862,  "tagfs tag exists",              "tagfs") \
    X(TAGFS_INVALID_TAG,             tagfs_invalid_tag,             863,  "tagfs invalid tag",             "tagfs") \
    X(TAGFS_EXTENT_ERROR,            tagfs_extent_error,            864,  "tagfs extent error",            "tagfs") \
    X(TAGFS_RECOVERY_FAILED,         tagfs_recovery_failed,         865,  "tagfs recovery failed",         "tagfs") \
     \
    X(POCKET_RING_FULL,              pocket_ring_full,              900,  "pocket ring full",              "ipc") \
    X(RESULT_RING_FULL,              result_ring_full,              901,  "result ring full",              "ipc") \
    X(INVALID_POCKET,                invalid_pocket,                902,  "invalid pocket",                "ipc") \
    X(INVALID_DECK_ID,               invalid_deck_id,               903,  "invalid deck id",               "ipc") \
    X(INVALID_OPCODE,                invalid_opcode,                904,  "invalid opcode",                "ipc") \
    X(PREFIX_CHAIN_TOO_LONG,         prefix_chain_too_long,         905,  "prefix chain too long",         "ipc") \
    X(POCKET_PROCESSING_FAILED,      pocket_processing_failed,      906,  "pocket processing failed",      "ipc") \
    X(PENDING_QUEUE_FULL,            pending_queue_full,            907,  "pending queue full",            "ipc") \
    X(KCORE_QUEUE_FULL,              kcore_queue_full,              908,  "k-core queue full",             "ipc") \
    X(KCORE_SUBMIT_FAILED,           kcore_submit_failed,           909,  "k-core submit failed",          "ipc") \
    X(RESULT_NOT_READY,              result_not_ready,              910,  "result not ready",              "ipc") \
    X(RESULT_STASH_FULL,             result_stash_full,             911,  "result stash full",             "ipc") \
     \
    X(ROUTE_TARGET_FULL,             route_target_full,             940,  "route target queue full",       "routing") \
    X(ROUTE_NO_SUBSCRIBERS,          route_no_subscribers,          941,  "no subscribers for route",      "routing") \
    X(ROUTE_SELF,                    route_self,                    942,  "cannot route to self",          "routing") \
    X(LISTEN_TABLE_FULL,             listen_table_full,             943,  "listen table full",             "routing") \
    X(LISTEN_ALREADY,                listen_already,                944,  "already listening on route",    "routing") \
    X(ROUTE_INVALID_TAG,             route_invalid_tag,             945,  "invalid route tag",             "routing") \
     \
    X(POCKET_FAILED,                 pocket_failed,                 950,  "pocket failed",                 "ipc") \
     \
    X(SCHEDULER_LOCKED,              scheduler_locked,              960,  "scheduler locked",              "scheduler") \
    X(RUNQUEUE_FULL,                 runqueue_full,                 961,  "runqueue full",                 "scheduler") \
    X(RUNQUEUE_EMPTY,                runqueue_empty,                962,  "runqueue empty",                "scheduler") \
    X(HOME_CORE_INVALID,             home_core_invalid,             963,  "invalid home core",             "scheduler") \
    X(WORK_STEAL_FAILED,             work_steal_failed,             964,  "work stealing failed",          "scheduler") \
     \
    X(BOOT_INFO_INVALID,             boot_info_invalid,             970,  "invalid boot info",             "boot") \
    X(E820_FAILED,                   e820_failed,                   971,  "e820 memory detection failed",  "boot") \
    X(A20_FAILED,                    a20_failed,                    972,  "a20 gate failed",               "boot") \
    X(LONG_MODE_FAILED,              long_mode_failed,              973,  "long mode failed",              "boot") \
    X(KERNEL_LOAD_FAILED,            kernel_load_failed,            974,  "kernel load failed",            "boot") \
     \
    X(DISKBOOK_NOT_INITIALIZED,      diskbook_not_initialized,      1000, "diskbook not initialized",      "diskbook") \
    X(DISKBOOK_FULL,                 diskbook_full,                 1001, "diskbook journal full",         "diskbook") \
    X(DISKBOOK_CORRUPTED,            diskbook_corrupted,            1002, "diskbook journal corrupted",    "diskbook") \
    X(DISKBOOK_COMMIT_FAILED,        diskbook_commit_failed,        1003, "diskbook commit failed",        "diskbook") \
    X(DISKBOOK_CHECKPOINT_FAILED,    diskbook_checkpoint_failed,    1004, "diskbook checkpoint failed",    "diskbook") \
    X(DISKBOOK_REPLAY_FAILED,        diskbook_replay_failed,        1005, "diskbook replay failed",        "diskbook") \
    X(DISKBOOK_INVALID_TXN,          diskbook_invalid_txn,          1006, "diskbook invalid transaction",  "diskbook") \
    X(DISKBOOK_WRITE_FAILED,         diskbook_write_failed,         1007, "diskbook write failed",         "diskbook") \
    X(DISKBOOK_READ_FAILED,          diskbook_read_failed,          1008, "diskbook read failed",          "diskbook") \
     \
    X(COW_NOT_INITIALIZED,           cow_not_initialized,           1010, "cow snapshots not initialized", "cow") \
    X(COW_SNAPSHOT_EXISTS,           cow_snapshot_exists,           1011, "snapshot already exists",       "cow") \
    X(COW_SNAPSHOT_NOT_FOUND,        cow_snapshot_not_found,        1012, "snapshot not found",            "cow") \
    X(COW_SNAPSHOT_LIMIT,            cow_snapshot_limit,            1013, "snapshot limit reached",        "cow") \
    X(COW_ALLOCATION_FAILED,         cow_allocation_failed,         1014, "cow allocation failed",         "cow") \
    X(COW_RESTORE_FAILED,            cow_restore_failed,            1015, "snapshot restore failed",       "cow") \
     \
    X(DEDUP_NOT_INITIALIZED,         dedup_not_initialized,         1020, "dedup not initialized",         "dedup") \
    X(DEDUP_HASH_COLLISION,          dedup_hash_collision,          1021, "dedup hash collision",          "dedup") \
    X(DEDUP_POOL_EXHAUSTED,          dedup_pool_exhausted,          1022, "dedup entry pool exhausted",    "dedup") \
    X(DEDUP_GC_FAILED,               dedup_gc_failed,               1023, "dedup gc failed",               "dedup") \
    X(DEDUP_REGISTER_FAILED,         dedup_register_failed,         1024, "dedup register failed",         "dedup") \
     \
    X(SELF_HEAL_NOT_INITIALIZED,     self_heal_not_initialized,     1030, "self-heal not initialized",     "selfheal") \
    X(SELF_HEAL_CORRUPTION_DETECTED, self_heal_corruption_detected, 1031, "self-heal corruption detected", "selfheal") \
    X(SELF_HEAL_RECOVERY_FAILED,     self_heal_recovery_failed,     1032, "self-heal recovery failed",     "selfheal") \
    X(SELF_HEAL_MIRROR_FAILED,       self_heal_mirror_failed,       1033, "self-heal mirror failed",       "selfheal") \
    X(SELF_HEAL_SCRUB_FAILED,        self_heal_scrub_failed,        1034, "self-heal scrub failed",        "selfheal") \
     \
    X(BOXHASH_INVALID_CONTEXT,       boxhash_invalid_context,       1040, "boxhash invalid context",       "boxhash") \
    X(BOXHASH_VERIFICATION_FAILED,   boxhash_verification_failed,   1041, "boxhash verification failed",   "boxhash") \
    X(BOXHASH_KEY_NOT_SET,           boxhash_key_not_set,           1042, "boxhash key not set",           "boxhash") \
     \
    X(BRAID_NOT_INITIALIZED,         braid_not_initialized,         1050, "braid not initialized",         "braid") \
    X(BRAID_DISK_OFFLINE,            braid_disk_offline,            1051, "braid disk offline",            "braid") \
    X(BRAID_DISK_FULL,               braid_disk_full,               1052, "braid disk full",               "braid") \
    X(BRAID_READ_FAILED,             braid_read_failed,             1053, "braid read failed",             "braid") \
    X(BRAID_WRITE_FAILED,            braid_write_failed,            1054, "braid write failed",            "braid") \
    X(BRAID_CHECKSUM_MISMATCH,       braid_checksum_mismatch,       1055, "braid checksum mismatch",       "braid") \
    X(BRAID_HEAL_FAILED,             braid_heal_failed,             1056, "braid heal failed",             "braid") \
    X(BRAID_INSUFFICIENT_DISKS,      braid_insufficient_disks,      1057, "braid insufficient disks",      "braid") \
    X(BRAID_MODE_INVALID,            braid_mode_invalid,            1058, "braid invalid mode",            "braid") \
    X(BRAID_REBUILD_FAILED,          braid_rebuild_failed,          1059, "braid rebuild failed",          "braid") \
     \
    X(ADDR_VALUE_MISMATCH,           addr_value_mismatch,           1100, "park value mismatch",           "strand")

enum {
#define BOX_ERROR_DEFINE(SUFFIX, name, value, msg, cat) ERR_##SUFFIX = (value),
    BOX_ERROR_LIST(BOX_ERROR_DEFINE)
#undef BOX_ERROR_DEFINE
};

#define ERR_INVALID_ARGS        ERR_INVALID_ARGUMENT
#define ERR_RESULT_INVALID      ERR_CORRUPTED

#define IS_ERROR(err)   ((err) != OK)
#define IS_SUCCESS(err) ((err) == OK)

static inline int box_fail(int rc)
{
    return rc == 0 ? 0 : (rc > 0 ? -rc : rc);
}

static inline error_t box_errno_of(int ret)
{
    return ret >= 0 ? OK : (error_t)(-(long)ret);
}

#ifdef __cplusplus
}
#endif

#endif