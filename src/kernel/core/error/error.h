#ifndef BOXOS_ERROR_H
#define BOXOS_ERROR_H

#include "ktypes.h"

// BoxOS Unified Error Code System
// Error codes are organized by category with reserved ranges for future expansion

typedef enum {
    // Success (0)
    BOXOS_OK = 0,

    // General Errors (1-99)
    BOXOS_ERR_UNKNOWN = 1,
    BOXOS_ERR_NOT_IMPLEMENTED = 2,
    BOXOS_ERR_INVALID_ARGUMENT = 3,
    BOXOS_ERR_NULL_POINTER = 4,
    BOXOS_ERR_OUT_OF_RANGE = 5,
    BOXOS_ERR_BUFFER_TOO_SMALL = 6,
    BOXOS_ERR_TIMEOUT = 7,
    BOXOS_ERR_BUSY = 8,
    BOXOS_ERR_WOULD_BLOCK = 9,
    BOXOS_ERR_ALIGNMENT = 10,
    BOXOS_ERR_CORRUPTED = 11,
    BOXOS_ERR_INTERNAL = 12,

    // Memory Errors (100-199)
    BOXOS_ERR_NO_MEMORY = 100,
    BOXOS_ERR_INVALID_ADDRESS = 101,
    BOXOS_ERR_PAGE_FAULT = 102,
    BOXOS_ERR_ALREADY_MAPPED = 103,
    BOXOS_ERR_NOT_MAPPED = 104,
    BOXOS_ERR_PERMISSION_DENIED_MEM = 105,
    BOXOS_ERR_HEAP_EXHAUSTED = 106,
    BOXOS_ERR_STACK_OVERFLOW = 107,
    BOXOS_ERR_BUFFER_OVERFLOW = 108,
    BOXOS_ERR_INVALID_BUFFER_ID = 109,
    BOXOS_ERR_BUFFER_IN_USE = 110,
    BOXOS_ERR_BUFFER_LIMIT_EXCEEDED = 111,

    // I/O Errors (200-299)
    BOXOS_ERR_IO = 200,
    BOXOS_ERR_READ_FAILED = 201,
    BOXOS_ERR_WRITE_FAILED = 202,
    BOXOS_ERR_DEVICE_NOT_READY = 203,
    BOXOS_ERR_DEVICE_ERROR = 204,
    BOXOS_ERR_END_OF_FILE = 205,
    BOXOS_ERR_DISK_FULL = 206,
    BOXOS_ERR_BAD_SECTOR = 207,
    BOXOS_ERR_IO_PENDING = 208,
    BOXOS_ERR_IO_QUEUE_FULL = 209,
    BOXOS_ERR_IO_CANCELLED = 211,

    // Filesystem Errors (300-399)
    BOXOS_ERR_FILE_NOT_FOUND = 300,
    BOXOS_ERR_OBJECT_NOT_FOUND = 301,
    BOXOS_ERR_TAG_NOT_FOUND = 302,
    BOXOS_ERR_ALREADY_EXISTS = 303,
    BOXOS_ERR_INVALID_TAG = 304,
    BOXOS_ERR_TAG_LIMIT_EXCEEDED = 305,
    BOXOS_ERR_OBJECT_CORRUPTED = 306,
    BOXOS_ERR_JOURNAL_FULL = 307,
    BOXOS_ERR_JOURNAL_CORRUPTED = 308,
    BOXOS_ERR_METADATA_CORRUPTED = 309,

    // Process Errors (400-499)
    BOXOS_ERR_PROCESS_NOT_FOUND = 400,
    BOXOS_ERR_INVALID_PID = 401,
    BOXOS_ERR_PROCESS_LIMIT_EXCEEDED = 402,
    BOXOS_ERR_PROCESS_TERMINATED = 403,
    BOXOS_ERR_PROCESS_BLOCKED = 404,
    BOXOS_ERR_INVALID_ELF = 405,
    BOXOS_ERR_BINARY_TOO_LARGE = 406,
    BOXOS_ERR_SPAWN_FAILED = 407,

    // Security Errors (500-599)
    BOXOS_ERR_ACCESS_DENIED = 500,
    BOXOS_ERR_PERMISSION_DENIED = 501,
    BOXOS_ERR_SECURITY_VIOLATION = 502,
    BOXOS_ERR_TAG_MISMATCH = 503,
    BOXOS_ERR_INVALID_OPERATION = 504,

    // Hardware Errors (600-699)
    BOXOS_ERR_HARDWARE = 600,
    BOXOS_ERR_INVALID_DEVICE = 601,
    BOXOS_ERR_DEVICE_BUSY = 602,
    BOXOS_ERR_KEYBOARD_BUFFER_FULL = 603,
    BOXOS_ERR_VGA_ERROR = 604,
    BOXOS_ERR_ATA_ERROR = 605,
    BOXOS_ERR_PCI_ERROR = 606,

    // Network Errors (700-799)
    // Reserved for future network stack

    // ACPI Errors (800-899)
    BOXOS_ERR_ACPI_NOT_FOUND = 800,
    BOXOS_ERR_ACPI_INVALID_TABLE = 801,
    BOXOS_ERR_ACPI_CHECKSUM_FAILED = 802,
    BOXOS_ERR_ACPI_PARSE_ERROR = 803,

    // Event System Errors (900-999)
    BOXOS_ERR_EVENT_RING_FULL = 900,
    BOXOS_ERR_RESULT_RING_FULL = 901,
    BOXOS_ERR_INVALID_EVENT = 902,
    BOXOS_ERR_INVALID_DECK_ID = 903,
    BOXOS_ERR_INVALID_OPCODE = 904,
    BOXOS_ERR_PREFIX_CHAIN_TOO_LONG = 905,
    BOXOS_ERR_EVENT_PROCESSING_FAILED = 906,
    BOXOS_ERR_PENDING_QUEUE_FULL = 907,

    // Routing/IPC Errors (940-949)
    BOXOS_ERR_ROUTE_TARGET_FULL    = 940,
    BOXOS_ERR_ROUTE_NO_SUBSCRIBERS = 941,
    BOXOS_ERR_ROUTE_SELF           = 942,
    BOXOS_ERR_LISTEN_TABLE_FULL    = 943,
    BOXOS_ERR_LISTEN_ALREADY       = 944,

    // Sentinel value (do not use)
    BOXOS_ERR_MAX = 999
} boxos_error_t;

// Helper macros
#define BOXOS_IS_ERROR(err) ((err) != BOXOS_OK)
#define BOXOS_IS_SUCCESS(err) ((err) == BOXOS_OK)

// Error propagation macro: return error if operation failed
#define BOXOS_PROPAGATE(expr) do { \
    boxos_error_t _err = (expr); \
    if (BOXOS_IS_ERROR(_err)) { \
        return _err; \
    } \
} while (0)

// Legacy conversion functions (for backward compatibility during migration)
static inline boxos_error_t boxos_from_legacy_int(int legacy_code) {
    if (legacy_code == 0) return BOXOS_OK;
    if (legacy_code == -1) return BOXOS_ERR_INTERNAL;
    if (legacy_code == -2) return BOXOS_ERR_INVALID_ARGUMENT;
    if (legacy_code == -3) return BOXOS_ERR_NO_MEMORY;
    if (legacy_code == -4) return BOXOS_ERR_FILE_NOT_FOUND;
    if (legacy_code == -5) return BOXOS_ERR_ACCESS_DENIED;
    if (legacy_code < 0) return BOXOS_ERR_UNKNOWN;
    return (boxos_error_t)legacy_code;
}

static inline int boxos_to_legacy_int(boxos_error_t err) {
    if (err == BOXOS_OK) return 0;
    if (err > 0) return -1;  // Treat all errors as generic -1 for old code
    return (int)err;
}

// ============================================================================
// DECK ERROR CODE MAPPINGS (for backward compatibility)
// ============================================================================
// These aliases allow deck-specific code to use semantic names
// while mapping to standard boxos_error_t values

// Storage Deck error codes
#define STORAGE_OK               BOXOS_OK
#define STORAGE_ERR_INVALID      BOXOS_ERR_INVALID_ARGUMENT
#define STORAGE_ERR_NOT_FOUND    BOXOS_ERR_FILE_NOT_FOUND
#define STORAGE_ERR_NO_SPACE     BOXOS_ERR_DISK_FULL
#define STORAGE_ERR_IO           BOXOS_ERR_IO
#define STORAGE_ERR_EXISTS       BOXOS_ERR_ALREADY_EXISTS
#define STORAGE_ERR_PERMISSION   BOXOS_ERR_ACCESS_DENIED
#define STORAGE_ERR_FILE_LIMIT   BOXOS_ERR_BUFFER_LIMIT_EXCEEDED
#define STORAGE_ERR_TAG_INVALID  BOXOS_ERR_INVALID_TAG
#define STORAGE_ERR_TAG_LIMIT    BOXOS_ERR_TAG_LIMIT_EXCEEDED

// Hardware Deck error codes
#define HW_OK                    BOXOS_OK
#define HW_ERR_NOT_FOUND         BOXOS_ERR_INVALID_DEVICE
#define HW_ERR_TIMEOUT           BOXOS_ERR_TIMEOUT
#define HW_ERR_IO                BOXOS_ERR_IO
#define HW_ERR_BUSY              BOXOS_ERR_DEVICE_BUSY

// Operations Deck error codes
#define OPS_OK                   BOXOS_OK
#define OPS_ERR_INVALID          BOXOS_ERR_INVALID_ARGUMENT
#define OPS_ERR_NOT_FOUND        BOXOS_ERR_OBJECT_NOT_FOUND

// System Deck error codes
#define SYS_OK                   BOXOS_OK
#define SYS_ERR_INVALID          BOXOS_ERR_INVALID_ARGUMENT
#define SYS_ERR_NO_MEMORY        BOXOS_ERR_NO_MEMORY
#define SYS_ERR_NOT_FOUND        BOXOS_ERR_PROCESS_NOT_FOUND

// Convert error code to human-readable string
const char* boxos_error_string(boxos_error_t err);

#endif // BOXOS_ERROR_H
