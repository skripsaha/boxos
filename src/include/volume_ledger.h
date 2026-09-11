#ifndef VOLUME_LEDGER_H
#define VOLUME_LEDGER_H



#define VOLUME_LEDGER_MAGIC_0  'B'
#define VOLUME_LEDGER_MAGIC_1  'O'
#define VOLUME_LEDGER_MAGIC_2  'X'
#define VOLUME_LEDGER_MAGIC_3  'L'
#define VOLUME_LEDGER_MAGIC_4  'E'
#define VOLUME_LEDGER_MAGIC_5  'D'
#define VOLUME_LEDGER_MAGIC_6  'G'
#define VOLUME_LEDGER_MAGIC_7  'R'

#define VOLUME_LEDGER_COPIES  2u

typedef struct __attribute__((packed)) {
    uint8_t  magic[8];
    uint32_t bytes;
    uint32_t crc32;
    uint64_t seq;

    uint64_t written_unix;
    uint64_t free_blocks;
    uint64_t total_files;
    uint32_t next_file_id;
    uint32_t next_tag_id;
    uint32_t total_tags;

    uint32_t cow_manifest_block;
    uint32_t cow_manifest_backup_block;

    uint32_t integrity_map_block;
    uint32_t integrity_map_blocks;

    uint16_t use_context_offset;
    uint16_t use_context_bytes;
} VolumeLedger;

#define VOLUME_LEDGER_REQUIRED_BYTES  __builtin_offsetof(VolumeLedger, use_context_offset)

#define VOLUME_LEDGER_NO_BLOCK  0u

#endif