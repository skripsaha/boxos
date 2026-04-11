#ifndef BOX_ERROR_H
#define BOX_ERROR_H

#include "types.h"

typedef uint32_t error_t;

#define OK                      0

#define ERR_UNKNOWN             1
#define ERR_NOT_IMPLEMENTED     2
#define ERR_INVALID_ARGUMENT    3
#define ERR_NULL_POINTER        4
#define ERR_OUT_OF_RANGE        5
#define ERR_BUFFER_TOO_SMALL    6
#define ERR_TIMEOUT             7
#define ERR_BUSY                8
#define ERR_WOULD_BLOCK         9
#define ERR_ALIGNMENT           10
#define ERR_CORRUPTED           11
#define ERR_INTERNAL            12

#define ERR_NO_MEMORY           100
#define ERR_INVALID_ADDRESS     101
#define ERR_PAGE_FAULT          102
#define ERR_ALREADY_MAPPED      103
#define ERR_NOT_MAPPED          104
#define ERR_PERMISSION_DENIED_MEM 105
#define ERR_HEAP_EXHAUSTED      106
#define ERR_STACK_OVERFLOW      107
#define ERR_BUFFER_OVERFLOW     108
#define ERR_INVALID_BUFFER_ID   109
#define ERR_BUFFER_IN_USE       110
#define ERR_BUFFER_LIMIT_EXCEEDED 111

#define ERR_IO                  200
#define ERR_READ_FAILED         201
#define ERR_WRITE_FAILED        202
#define ERR_DEVICE_NOT_READY    203
#define ERR_DEVICE_ERROR        204
#define ERR_END_OF_FILE         205
#define ERR_DISK_FULL           206
#define ERR_BAD_SECTOR          207

#define ERR_FILE_NOT_FOUND      300
#define ERR_OBJECT_NOT_FOUND    301
#define ERR_TAG_NOT_FOUND       302
#define ERR_ALREADY_EXISTS      303
#define ERR_INVALID_TAG         304
#define ERR_TAG_LIMIT_EXCEEDED  305
#define ERR_OBJECT_CORRUPTED    306
#define ERR_JOURNAL_FULL        307
#define ERR_JOURNAL_CORRUPTED   308
#define ERR_METADATA_CORRUPTED  309

#define ERR_PROCESS_NOT_FOUND   400
#define ERR_INVALID_PID         401
#define ERR_PROCESS_LIMIT_EXCEEDED 402
#define ERR_PROCESS_TERMINATED  403
#define ERR_PROCESS_BLOCKED     404
#define ERR_INVALID_ELF         405
#define ERR_BINARY_TOO_LARGE    406
#define ERR_SPAWN_FAILED        407

#define ERR_ACCESS_DENIED       500
#define ERR_PERMISSION_DENIED   501
#define ERR_SECURITY_VIOLATION  502
#define ERR_TAG_MISMATCH        503
#define ERR_INVALID_OPERATION   504

#define ERR_HARDWARE            600
#define ERR_INVALID_DEVICE      601
#define ERR_DEVICE_BUSY         602
#define ERR_KEYBOARD_BUFFER_FULL 603
#define ERR_VGA_ERROR           604
#define ERR_ATA_ERROR           605
#define ERR_PCI_ERROR           606

#define ERR_POCKET_RING_FULL    900
#define ERR_RESULT_RING_FULL    901
#define ERR_INVALID_POCKET      902
#define ERR_INVALID_DECK_ID     903
#define ERR_INVALID_OPCODE      904
#define ERR_PREFIX_CHAIN_TOO_LONG 905
#define ERR_POCKET_PROCESSING_FAILED 906
#define ERR_PENDING_QUEUE_FULL  907

#define ERR_INVALID_ARGS        ERR_INVALID_ARGUMENT
#define ERR_POCKET_FAILED       ERR_POCKET_PROCESSING_FAILED
#define ERR_RESULT_INVALID      ERR_CORRUPTED

#define IS_ERROR(err)   ((err) != OK)
#define IS_SUCCESS(err) ((err) == OK)

#endif // BOX_ERROR_H
