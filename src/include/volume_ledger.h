#ifndef VOLUME_LEDGER_H
#define VOLUME_LEDGER_H

/*
 * The Ledger — what is true of a volume today, as opposed to what it is.
 *
 * The Deed (volume_deed.h) says which volume this is, how far it runs and
 * where its parts are. All of that is settled when the volume is made. This is
 * the other half: how many blocks are left, which id the next file gets, how
 * many files there are, when it was last written to. Every one of those
 * changes when somebody saves a byte.
 *
 * They were one record until now — a 512-byte superblock carrying the
 * volume's identity, its layout AND its counters, rewritten with a fresh
 * checksum on every allocation. So the identity of the volume was put at risk
 * by ordinary use, and its backup copy — written in the same breath, one
 * sector later, inside the same 4096-byte physical block — was at exactly the
 * same risk at exactly the same moment.
 *
 *
 * ‼ TWO COPIES, ALTERNATELY, EACH SAYING WHICH IS NEWER
 *
 * A record rewritten continuously is the one record that will be found
 * half-written: power goes at the wrong microsecond, or the flash translation
 * layer is mid-erase, and the sector reads back as neither the old value nor
 * the new one.
 *
 * So there are two, in two separate blocks of the volume (`state_block` and
 * `state_block + 1`), and a write goes to whichever one is currently OLDER.
 * The newer copy is never the one being overwritten, which means that at every
 * instant, including every instant during a write, at least one intact Ledger
 * exists on the medium. `seq` counts up forever and settles which that is; the
 * checksum settles whether a copy is intact at all.
 *
 * That is the entire recovery story, and it needs no journal, no replay and no
 * repair pass — a torn Ledger is not damage to be fixed, it is simply the copy
 * that loses.
 *
 *
 * ‼ HOW THIS GROWS
 *
 * `bytes` is how much of the record the writer wrote and the checksum covers.
 * A reader requires it to be at least as long as the fields it knows, and
 * ignores anything past them — so a volume written by a newer kernel mounts on
 * an older one, and the fields it does not know are simply not read.
 *
 * That is why there is no `reserved[]` here and no version number. A version
 * ladder answers "may I read this at all"; a length answers "how much of this
 * do I understand", which is the question that actually gets asked.
 */

/*
 * Integer types come from the includer, as everywhere in src/include:
 *   Kernel:    #include "ktypes.h"
 *   Host tool: #include <stdint.h>
 * See the same note in volume_deed.h for why this header chooses neither.
 */

/* 'B','O','X','L','E','D','G','R' — reads as words in a hex dump, like the
 * Deed's, and is a different word from it: a block that is one when it should
 * be the other is a misdirected read and says so immediately. */
#define VOLUME_LEDGER_MAGIC_0  'B'
#define VOLUME_LEDGER_MAGIC_1  'O'
#define VOLUME_LEDGER_MAGIC_2  'X'
#define VOLUME_LEDGER_MAGIC_3  'L'
#define VOLUME_LEDGER_MAGIC_4  'E'
#define VOLUME_LEDGER_MAGIC_5  'D'
#define VOLUME_LEDGER_MAGIC_6  'G'
#define VOLUME_LEDGER_MAGIC_7  'R'

/* How many copies the format keeps. Named rather than spelled as 2 in the four
 * places that would otherwise each say 2: the mkfs that lays them out, the
 * mount that reads them, the write that alternates between them, and the Deed
 * that reserves the room. */
#define VOLUME_LEDGER_COPIES  2u

typedef struct __attribute__((packed)) {
    uint8_t  magic[8];
    uint32_t bytes;             /* how much of this record exists and is summed */
    uint32_t crc32;             /* over `bytes`, with this field zeroed */
    uint64_t seq;               /* higher wins; never reused, never reset */

    uint64_t written_unix;      /* when this copy was made */
    uint64_t free_blocks;       /* of the volume's data run */
    uint64_t total_files;
    uint32_t next_file_id;
    uint32_t next_tag_id;
    uint32_t total_tags;

    /* Where the CoW snapshot manifest and its backup live. Zero until the
     * first snapshot is taken — they are allocated out of the data run when
     * they are first needed, which makes them state and not layout, and state
     * is what this record is for. */
    uint32_t cow_manifest_block;
    uint32_t cow_manifest_backup_block;

    /* And the integrity map — the digests that catch a block that came back
     * changed. Allocated lazily for the same reason, and lost with the same
     * consequence: a volume that forgets where it put them re-derives them,
     * it does not lose data.
     *
     * These four were carved out of the old superblock's `reserved[]` by
     * comment — bytes 16..23 for the manifest, 24..31 for the map — with two
     * subsystems each trusting the other to stay inside its stated range. */
    uint32_t integrity_map_block;
    uint32_t integrity_map_blocks;
} VolumeLedger;

/* Blocks in the data run are counted from zero, so zero cannot also mean "a
 * block". Nothing is allocated at data block 0 by anybody: mkfs puts the tag
 * registry there and marks it used before a volume is ever mounted. */
#define VOLUME_LEDGER_NO_BLOCK  0u

#endif /* VOLUME_LEDGER_H */
