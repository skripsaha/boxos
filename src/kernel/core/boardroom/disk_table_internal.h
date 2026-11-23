#ifndef DISK_TABLE_INTERNAL_H
#define DISK_TABLE_INTERNAL_H

#include "disk_table.h"

/*
 * What the reading and the writing of a disk's table (disk_table.c,
 * disk_table_write.c) must agree on: the shape of a GPT on the medium, UEFI
 * 2.10 §5.3, and how a copy of it is read.
 */

#define GPT_HEADER_LBA            1u
#define GPT_SIG_OFFSET            0x00u
#define GPT_REVISION_OFFSET       0x08u
#define GPT_HEADER_SIZE_OFFSET    0x0Cu
#define GPT_HEADER_CRC_OFFSET     0x10u
#define GPT_MY_LBA_OFFSET         0x18u
#define GPT_ALT_LBA_OFFSET        0x20u
#define GPT_FIRST_USABLE_OFFSET   0x28u
#define GPT_LAST_USABLE_OFFSET    0x30u
#define GPT_DISK_GUID_OFFSET      0x38u
#define GPT_ENTRY_LBA_OFFSET      0x48u
#define GPT_ENTRY_COUNT_OFFSET    0x50u
#define GPT_ENTRY_BYTES_OFFSET    0x54u
#define GPT_ENTRY_CRC_OFFSET      0x58u
#define GPT_HEADER_MIN_BYTES      92u
#define GPT_REVISION              0x00010000u

#define GPT_ENTRY_TYPE_OFFSET     0x00u
#define GPT_ENTRY_UNIQUE_OFFSET   0x10u
#define GPT_ENTRY_FIRST_OFFSET    0x20u
#define GPT_ENTRY_LAST_OFFSET     0x28u
#define GPT_ENTRY_ATTR_OFFSET     0x30u
#define GPT_ENTRY_NAME_OFFSET     0x38u
#define GPT_ENTRY_NAME_UNITS      36u
#define GPT_ENTRY_MIN_BYTES       128u

/* The ceiling on an array this reads, so a header stating an absurd one
 * cannot be talked into a huge allocation or an overflowing multiply; every
 * tool in use writes 128 entries of 128 bytes, 16 KiB. */
#define GPT_ARRAY_MAX_BYTES       (128u * 1024u)
/* What this writes: 128 entries of 128 bytes, as every tool does — the
 * least the specification reserves (16 KiB). */
#define GPT_WRITE_ENTRIES         128u
#define GPT_WRITE_ENTRY_BYTES     128u

/* What reading one copy told: "nothing of a table here" and "a table that is
 * not to be believed" are opposite answers, and only the second is worth
 * crossing the medium to the other copy for. */
typedef enum {
    DISK_GPT_READ = 0,   /* believed */
    DISK_GPT_ABSENT,     /* no signature there */
    DISK_GPT_DAMAGED,    /* there is one, and it does not hold */
    DISK_GPT_REFUSED,    /* the medium would not give it up */
} DiskGptVerdict;

typedef struct {
    DiskGptVerdict verdict;
    uint64_t       header_block;
    uint64_t       alt_block;
    uint64_t       first_usable;
    uint64_t       last_usable;
    uint8_t        disk_guid[16];
    uint64_t       entry_block;
    uint32_t       entry_count;
    uint32_t       entry_bytes;
    uint32_t       array_crc;
    uint8_t*       array;          /* owned; NULL unless the copy holds */
} DiskGptCopy;

uint32_t       DiskLe32(const uint8_t* p);
uint64_t       DiskLe64(const uint8_t* p);
int            DiskReadBlocks(uint8_t seat, uint32_t block_bytes, uint64_t block, uint64_t count, void* buf);
DiskGptVerdict DiskGptCopyRead(uint8_t seat, uint32_t block_bytes, uint64_t blocks, uint64_t header_block,
                               DiskGptCopy* out);
void           DiskGptCopyLetGo(DiskGptCopy* c);

/* A write to some disk's table under way: begun before its first block goes
 * out, ended after the last is read back and what was remembered of its disk
 * let go. A read of a medium that a write overlapped is answered and not
 * remembered — it may have met half of the write. */
void           DiskWriteBegins(void);
void           DiskWriteEnds(uint8_t seat);

#endif /* DISK_TABLE_INTERNAL_H */
