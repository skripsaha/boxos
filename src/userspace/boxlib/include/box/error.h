#ifndef BOX_ERROR_H
#define BOX_ERROR_H

#include "types.h"

// BoxLib Error Code System
// Mirrors kernel boxos_error_t for userspace applications
typedef uint32_t box_error_t;

// Success
#define BOX_OK                      0

// General Errors (1-99)
#define BOX_ERR_UNKNOWN             1
#define BOX_ERR_NOT_IMPLEMENTED     2
#define BOX_ERR_INVALID_ARGUMENT    3
#define BOX_ERR_NULL_POINTER        4
#define BOX_ERR_OUT_OF_RANGE        5
#define BOX_ERR_BUFFER_TOO_SMALL    6
#define BOX_ERR_TIMEOUT             7
#define BOX_ERR_BUSY                8
#define BOX_ERR_WOULD_BLOCK         9
#define BOX_ERR_ALIGNMENT           10
#define BOX_ERR_CORRUPTED           11
#define BOX_ERR_INTERNAL            12

// Memory Errors (100-199)
#define BOX_ERR_NO_MEMORY           100
#define BOX_ERR_INVALID_ADDRESS     101
#define BOX_ERR_PAGE_FAULT          102
#define BOX_ERR_ALREADY_MAPPED      103
#define BOX_ERR_NOT_MAPPED          104
#define BOX_ERR_PERMISSION_DENIED_MEM 105
#define BOX_ERR_HEAP_EXHAUSTED      106
#define BOX_ERR_STACK_OVERFLOW      107
#define BOX_ERR_BUFFER_OVERFLOW     108
#define BOX_ERR_INVALID_BUFFER_ID   109
#define BOX_ERR_BUFFER_IN_USE       110
#define BOX_ERR_BUFFER_LIMIT_EXCEEDED 111

// I/O Errors (200-299)
#define BOX_ERR_IO                  200
#define BOX_ERR_READ_FAILED         201
#define BOX_ERR_WRITE_FAILED        202
#define BOX_ERR_DEVICE_NOT_READY    203
#define BOX_ERR_DEVICE_ERROR        204
#define BOX_ERR_END_OF_FILE         205
#define BOX_ERR_DISK_FULL           206
#define BOX_ERR_BAD_SECTOR          207

// Filesystem Errors (300-399)
#define BOX_ERR_FILE_NOT_FOUND      300
#define BOX_ERR_OBJECT_NOT_FOUND    301
#define BOX_ERR_TAG_NOT_FOUND       302
#define BOX_ERR_ALREADY_EXISTS      303
#define BOX_ERR_INVALID_TAG         304
#define BOX_ERR_TAG_LIMIT_EXCEEDED  305
#define BOX_ERR_OBJECT_CORRUPTED    306
#define BOX_ERR_JOURNAL_FULL        307
#define BOX_ERR_JOURNAL_CORRUPTED   308
#define BOX_ERR_METADATA_CORRUPTED  309

// Process Errors (400-499)
#define BOX_ERR_PROCESS_NOT_FOUND   400
#define BOX_ERR_INVALID_PID         401
#define BOX_ERR_PROCESS_LIMIT_EXCEEDED 402
#define BOX_ERR_PROCESS_TERMINATED  403
#define BOX_ERR_PROCESS_BLOCKED     404
#define BOX_ERR_INVALID_ELF         405
#define BOX_ERR_BINARY_TOO_LARGE    406
#define BOX_ERR_SPAWN_FAILED        407

// Security Errors (500-599)
#define BOX_ERR_ACCESS_DENIED       500
#define BOX_ERR_PERMISSION_DENIED   501
#define BOX_ERR_SECURITY_VIOLATION  502
#define BOX_ERR_TAG_MISMATCH        503
#define BOX_ERR_INVALID_OPERATION   504

// Hardware Errors (600-699)
#define BOX_ERR_HARDWARE            600
#define BOX_ERR_INVALID_DEVICE      601
#define BOX_ERR_DEVICE_BUSY         602
#define BOX_ERR_KEYBOARD_BUFFER_FULL 603
#define BOX_ERR_VGA_ERROR           604
#define BOX_ERR_ATA_ERROR           605
#define BOX_ERR_PCI_ERROR           606

// Event System Errors (900-999)
#define BOX_ERR_EVENT_RING_FULL     900
#define BOX_ERR_RESULT_RING_FULL    901
#define BOX_ERR_INVALID_EVENT       902
#define BOX_ERR_INVALID_DECK_ID     903
#define BOX_ERR_INVALID_OPCODE      904
#define BOX_ERR_PREFIX_CHAIN_TOO_LONG 905
#define BOX_ERR_EVENT_PROCESSING_FAILED 906
#define BOX_ERR_PENDING_QUEUE_FULL  907

// Legacy compatibility aliases
#define BOX_ERR_INVALID_ARGS        BOX_ERR_INVALID_ARGUMENT
#define BOX_ERR_EVENT_FAILED        BOX_ERR_EVENT_PROCESSING_FAILED
#define BOX_ERR_RESULT_INVALID      BOX_ERR_CORRUPTED

// Helper macros
#define BOX_IS_ERROR(err)   ((err) != BOX_OK)
#define BOX_IS_SUCCESS(err) ((err) == BOX_OK)

#endif // BOX_ERROR_H
