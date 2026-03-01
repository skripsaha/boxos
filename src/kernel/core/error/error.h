#ifndef BOXOS_ERROR_H
#define BOXOS_ERROR_H

#include "ktypes.h"

typedef enum {
    OK = 0,

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
    ERR_ALIGNMENT = 10,
    ERR_CORRUPTED = 11,
    ERR_INTERNAL = 12,

    // Memory Errors (100-199)
    ERR_NO_MEMORY = 100,
    ERR_INVALID_ADDRESS = 101,
    ERR_PAGE_FAULT = 102,
    ERR_ALREADY_MAPPED = 103,
    ERR_NOT_MAPPED = 104,
    ERR_PERMISSION_DENIED_MEM = 105,
    ERR_HEAP_EXHAUSTED = 106,
    ERR_STACK_OVERFLOW = 107,
    ERR_BUFFER_OVERFLOW = 108,
    ERR_INVALID_BUFFER_ID = 109,
    ERR_BUFFER_IN_USE = 110,
    ERR_BUFFER_LIMIT_EXCEEDED = 111,

    // I/O Errors (200-299)
    ERR_IO = 200,
    ERR_READ_FAILED = 201,
    ERR_WRITE_FAILED = 202,
    ERR_DEVICE_NOT_READY = 203,
    ERR_DEVICE_ERROR = 204,
    ERR_END_OF_FILE = 205,
    ERR_DISK_FULL = 206,
    ERR_BAD_SECTOR = 207,
    ERR_IO_PENDING = 208,
    ERR_IO_QUEUE_FULL = 209,
    ERR_IO_CANCELLED = 211,

    // Filesystem Errors (300-399)
    ERR_FILE_NOT_FOUND = 300,
    ERR_OBJECT_NOT_FOUND = 301,
    ERR_TAG_NOT_FOUND = 302,
    ERR_ALREADY_EXISTS = 303,
    ERR_INVALID_TAG = 304,
    ERR_TAG_LIMIT_EXCEEDED = 305,
    ERR_OBJECT_CORRUPTED = 306,
    ERR_JOURNAL_FULL = 307,
    ERR_JOURNAL_CORRUPTED = 308,
    ERR_METADATA_CORRUPTED = 309,

    // Process Errors (400-499)
    ERR_PROCESS_NOT_FOUND = 400,
    ERR_INVALID_PID = 401,
    ERR_PROCESS_LIMIT_EXCEEDED = 402,
    ERR_PROCESS_TERMINATED = 403,
    ERR_PROCESS_BLOCKED = 404,
    ERR_INVALID_ELF = 405,
    ERR_BINARY_TOO_LARGE = 406,
    ERR_SPAWN_FAILED = 407,

    // Security Errors (500-599)
    ERR_ACCESS_DENIED = 500,
    ERR_PERMISSION_DENIED = 501,
    ERR_SECURITY_VIOLATION = 502,
    ERR_TAG_MISMATCH = 503,
    ERR_INVALID_OPERATION = 504,

    // Hardware Errors (600-699)
    ERR_HARDWARE = 600,
    ERR_INVALID_DEVICE = 601,
    ERR_DEVICE_BUSY = 602,
    ERR_KEYBOARD_BUFFER_FULL = 603,
    ERR_VGA_ERROR = 604,
    ERR_ATA_ERROR = 605,
    ERR_PCI_ERROR = 606,

    // Network Errors (700-799) - reserved

    // ACPI Errors (800-899)
    ERR_ACPI_NOT_FOUND = 800,
    ERR_ACPI_INVALID_TABLE = 801,
    ERR_ACPI_CHECKSUM_FAILED = 802,
    ERR_ACPI_PARSE_ERROR = 803,

    // Event System Errors (900-999)
    ERR_EVENT_RING_FULL = 900,
    ERR_RESULT_RING_FULL = 901,
    ERR_INVALID_EVENT = 902,
    ERR_INVALID_DECK_ID = 903,
    ERR_INVALID_OPCODE = 904,
    ERR_PREFIX_CHAIN_TOO_LONG = 905,
    ERR_EVENT_PROCESSING_FAILED = 906,
    ERR_PENDING_QUEUE_FULL = 907,

    // Routing/IPC Errors (940-949)
    ERR_ROUTE_TARGET_FULL    = 940,
    ERR_ROUTE_NO_SUBSCRIBERS = 941,
    ERR_ROUTE_SELF           = 942,
    ERR_LISTEN_TABLE_FULL    = 943,
    ERR_LISTEN_ALREADY       = 944,

    ERR_MAX = 999
} error_t;

#define IS_ERROR(err) ((err) != OK)
#define IS_SUCCESS(err) ((err) == OK)

#define PROPAGATE(expr) do { \
    error_t _err = (expr); \
    if (IS_ERROR(_err)) { \
        return _err; \
    } \
} while (0)

static inline error_t error_from_legacy_int(int legacy_code) {
    if (legacy_code == 0) return OK;
    if (legacy_code == -1) return ERR_INTERNAL;
    if (legacy_code == -2) return ERR_INVALID_ARGUMENT;
    if (legacy_code == -3) return ERR_NO_MEMORY;
    if (legacy_code == -4) return ERR_FILE_NOT_FOUND;
    if (legacy_code == -5) return ERR_ACCESS_DENIED;
    if (legacy_code < 0) return ERR_UNKNOWN;
    return (error_t)legacy_code;
}

static inline int error_to_legacy_int(error_t err) {
    if (err == OK) return 0;
    if (err > 0) return -1;
    return (int)err;
}

#define STORAGE_OK               OK
#define STORAGE_ERR_INVALID      ERR_INVALID_ARGUMENT
#define STORAGE_ERR_NOT_FOUND    ERR_FILE_NOT_FOUND
#define STORAGE_ERR_NO_SPACE     ERR_DISK_FULL
#define STORAGE_ERR_IO           ERR_IO
#define STORAGE_ERR_EXISTS       ERR_ALREADY_EXISTS
#define STORAGE_ERR_PERMISSION   ERR_ACCESS_DENIED
#define STORAGE_ERR_FILE_LIMIT   ERR_BUFFER_LIMIT_EXCEEDED
#define STORAGE_ERR_TAG_INVALID  ERR_INVALID_TAG
#define STORAGE_ERR_TAG_LIMIT    ERR_TAG_LIMIT_EXCEEDED

#define HW_OK                    OK
#define HW_ERR_NOT_FOUND         ERR_INVALID_DEVICE
#define HW_ERR_TIMEOUT           ERR_TIMEOUT
#define HW_ERR_IO                ERR_IO
#define HW_ERR_BUSY              ERR_DEVICE_BUSY

#define OPS_OK                   OK
#define OPS_ERR_INVALID          ERR_INVALID_ARGUMENT
#define OPS_ERR_NOT_FOUND        ERR_OBJECT_NOT_FOUND

#define SYS_OK                   OK
#define SYS_ERR_INVALID          ERR_INVALID_ARGUMENT
#define SYS_ERR_NO_MEMORY        ERR_NO_MEMORY
#define SYS_ERR_NOT_FOUND        ERR_PROCESS_NOT_FOUND

const char* error_string(error_t err);

#endif // BOXOS_ERROR_H
