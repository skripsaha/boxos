#ifndef BOXOS_DECKS_H
#define BOXOS_DECKS_H

#define DECK_EXECUTION     0x00
#define DECK_OPERATIONS    0x01
#define DECK_STORAGE       0x02
#define DECK_HARDWARE      0x03
#define DECK_NETWORK       0x04
#define DECK_SYSTEM        0xFF


#define SYSTEM_OP_PROC_SPAWN        0x01
#define SYSTEM_OP_PROC_KILL         0x02
#define SYSTEM_OP_PROC_INFO         0x03
#define SYSTEM_OP_PROC_CREW         0x0B
#define SYSTEM_OP_USE_SET           0x04
#define SYSTEM_OP_USE_GET           0x08
#define SYSTEM_OP_USE_CLEAR         0x09
#define SYSTEM_OP_PROC_CPUTIME      0x05
#define SYSTEM_OP_PROC_EXEC         0x06
#define SYSTEM_OP_INFO              0x07
#define SYSTEM_OP_TLS_FSBASE        0x0A

#define SYSTEM_OP_HEAP_PREFAULT     0x14

#define SYSTEM_OP_DEFRAG_FILE       0x18
#define SYSTEM_OP_FRAG_SCORE        0x19

#define SYSTEM_OP_TAG_ADD           0x20
#define SYSTEM_OP_TAG_REMOVE        0x21
#define SYSTEM_OP_TAG_CHECK         0x22

#define SYSTEM_OP_ROUTE             0x40
#define SYSTEM_OP_ROUTE_TAG         0x41

#define SYSTEM_OP_PERF_DUMP         0x50

#define SYSTEM_OP_TOUCH_CLAIM       0x51
#define SYSTEM_OP_TOUCH_RELEASE     0x52
#define SYSTEM_OP_TOUCH_SEND        0x53
#define SYSTEM_OP_TOUCH_AWAIT       0x54
#define SYSTEM_OP_TOUCH_IRQ_RETURN  0x55
#define SYSTEM_OP_TOUCH_REGISTER    0x56
#define SYSTEM_OP_TOUCH_ACK         0x57
#define SYSTEM_OP_TOUCH_INTERN      0x58

#define SYSTEM_OP_EFI_INFO          0x60
#define SYSTEM_OP_EFI_ESRT_GET      0x61
#define SYSTEM_OP_EFI_VERIFY_PE     0x62

#define SYSTEM_OP_BAY_OPEN          0x70
#define SYSTEM_OP_BAY_RELEASE       0x71
#define SYSTEM_OP_BAY_SIZE          0x72

#define SYSTEM_OP_BROOK_OPEN        0x75
#define SYSTEM_OP_BROOK_RELEASE     0x76
#define SYSTEM_OP_BROOK_INFO        0x7A

#define SYSTEM_OP_MANIFEST_COMPILE  0x80
#define SYSTEM_OP_MANIFEST_RELEASE  0x81


#define SYSTEM_OP_MEMTAG_QUERY      0xA0
#define SYSTEM_OP_MEMTAG_INFO       0xA1
#define SYSTEM_OP_MEMTAG_LOOKUP     0xA2
#define SYSTEM_OP_MEMTAG_TAGS       0xA3
#define SYSTEM_OP_MEMTAG_STATS      0xA4

#define SYSTEM_OP_MEMTAG_SET_GUARD  0xA5
#define SYSTEM_OP_MEMTAG_GRANT      0xA6
#define SYSTEM_OP_MEMTAG_REVOKE     0xA7
#define SYSTEM_OP_MEMTAG_CABIN_TAGS 0xA8
#define SYSTEM_OP_MEMTAG_CHECK      0xA9

#define SYSTEM_OP_MEMTAG_APPLY_PKEY 0xAA
#define SYSTEM_OP_MEMTAG_LOOKUP_VIRT 0xAB

#define SYSTEM_OP_HW_LAM_GET        0xB0
#define SYSTEM_OP_HW_LAM_SET        0xB1
#define SYSTEM_OP_HW_TME_STATE      0xB2

#define SYSTEM_OP_ADDR_PARK         0xC0
#define SYSTEM_OP_ADDR_WAKE         0xC1
#define SYSTEM_OP_STRAND_SPAWN      0xC2
#define SYSTEM_OP_STRAND_RELEASE    0xC3
#define SYSTEM_OP_STRAND_POOL_BIND  0xC4

#define SYSTEM_OP_PROCESS_GONE      0xC5

#define SYSTEM_OP_TURN_IN           0xC6

#define SYSTEM_OP_BELL              0xC7

#define STORAGE_TAG_QUERY           0x01
#define STORAGE_TAG_SET             0x02
#define STORAGE_TAG_UNSET           0x03
#define STORAGE_OBJ_READ            0x05
#define STORAGE_OBJ_WRITE           0x06
#define STORAGE_OBJ_CREATE          0x07
#define STORAGE_OBJ_DELETE          0x08
#define STORAGE_OBJ_RENAME          0x09
#define STORAGE_OBJ_GET_INFO        0x0A
#define STORAGE_OBJ_TRUNCATE        0x0B
#define STORAGE_SNAP_CREATE         0x20
#define STORAGE_SNAP_DELETE         0x21
#define STORAGE_SNAP_LIST           0x22
#define STORAGE_OBJ_ANCHOR          0x23
#define STORAGE_SNAP_INFO           0x24

#define STORAGE_SCOPE_USE           0
#define STORAGE_SCOPE_EVERYWHERE    1

#endif