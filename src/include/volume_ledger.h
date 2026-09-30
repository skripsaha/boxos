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
 *
 * Two things follow for anyone adding a field. The reader requires only the
 * fields it cannot do without (VOLUME_LEDGER_REQUIRED_BYTES) and reads a
 * shorter record as zeros past its end — otherwise the first field ever
 * added would have refused every volume made before it. And the record stays
 * inside its one sector: a reader takes one sector and refuses a longer
 * claim, so a record that outgrew the sector would be unreadable everywhere
 * it already mounts.
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

/*
 * ‼ THE SEAL OF THE LAST SETTLED CYCLE — SettleMark.
 *
 * The one writer of this record is a Settle cycle's S4, and it seals what the
 * cycle left on the medium: the metadata pool from `chain_first` to `pool_to`
 * and the tag registry up to `registry_*`, each with a checksum a mount holds
 * against the blocks it reads. A mount stands on the newest copy whose seal
 * the medium bears out and reads nothing of the pool past it: a cycle cut
 * anywhere before its record landed leaves the cycle before it, whole.
 *
 *   settle        the seq of the record the seal rides in; a seal is one only
 *                 when it says so (0 is none, and a copy of another record's
 *                 seal is none either);
 *   chain_first   the pool's first block — the authority on where the pool
 *                 begins; the Deed's head and its link are hints for readers
 *                 that know no seal, and the head is never unlinked;
 *   pool_from     where the cycle's own records begin: the pool_to of the
 *                 seal before it, or the tail as the mount found it — the
 *                 records a machine stopped uncleanly last wrote (RT-22);
 *   pool_to       the pool's open block, and the bytes of its records the
 *                 cycle wrote — nothing past them was settled;
 *   registry_*    the registry's last block on the medium, the bytes of its
 *                 records, and their CRC32;
 *   closed_crc32  the pool's closed blocks from chain_first up to pool_to's
 *                 block, folded: each block's CRC32 of its records, summed
 *                 again with the block's number and the records' length (a
 *                 12-byte {block, bytes, crc}), and the sums XORed — so
 *                 blocks dropped off the chain's front take their sums out and
 *                 nothing is read. A closed block is never written again by a
 *                 kernel that seals, so a closed part that does not match is a
 *                 kernel that retired records in place, or rot;
 *   tail_crc32    the open block's records up to pool_to;
 *   flags         SETTLE_CLEAN: written by a machine's last cycles as it
 *                 stopped; SETTLE_MEDIUM_LIED: a mount once found the medium
 *                 holding less than the newest seal said and stood on the one
 *                 before it — freed ground rests a cycle longer on it for good.
 */
#define SETTLE_CLEAN        (1u << 0)
#define SETTLE_MEDIUM_LIED  (1u << 1)

typedef struct __attribute__((packed)) {
    uint64_t settle;
    uint32_t chain_first;
    uint32_t pool_from_block;
    uint32_t pool_from_bytes;
    uint32_t pool_to_block;
    uint32_t pool_to_bytes;
    uint32_t registry_block;
    uint32_t registry_bytes;
    uint32_t registry_crc32;
    uint32_t closed_crc32;
    uint32_t tail_crc32;
    uint32_t flags;
} SettleMark;

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

    /* Where the CoW snapshot manifest and its backup live: zero unless a
     * kernel from before format v1 refused snapshots took one — they were
     * allocated out of the data run when first needed, which makes them
     * state and not layout, and state is what this record is for. */
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

    /* The Use Context the volume remembers — what its person was doing when
     * they left, said in tags, taken up again when the volume comes up under
     * a machine that holds none. The names ride inside this same record, one
     * comma-joined list after the fields (after the seal, from a kernel that
     * seals; right after these two, from one before): `use_context_offset`
     * from the start of the record, `use_context_bytes` long, no NUL. Both
     * zero when the volume remembers none. The record is one sector, and
     * that is the whole room a context has here — a list that will not fit
     * is not kept in part; the volume forgets, and the person is told. */
    uint16_t use_context_offset;
    uint16_t use_context_bytes;

    /* The growth field (HOW THIS GROWS): read only where LedgerHas says the
     * writer put it there. */
    SettleMark mark;
} VolumeLedger;

/* What every reader requires of `bytes`: the counters. A record written
 * before the Use Context was kept ends exactly here, and reads as a volume
 * that remembers none — the growth rule above, applied for the first time. */
#define VOLUME_LEDGER_REQUIRED_BYTES  __builtin_offsetof(VolumeLedger, use_context_offset)

/* Where the seal ends, and the one rule for every field of the growth field:
 * the record reaches past it, and no Use Context lies over it. A kernel
 * before the seal lays its context at the end of the two fields above —
 * exactly where the seal is — so its record, however long, never counts as
 * carrying one. */
#define VOLUME_LEDGER_SEAL_END  (__builtin_offsetof(VolumeLedger, mark) + sizeof(SettleMark))

static inline int LedgerHas(const VolumeLedger *led, uint32_t end)
{
    return led->bytes >= end &&
           (led->use_context_bytes == 0 || led->use_context_offset >= end);
}

/* The record is one sector for every reader: the fixed fields and the whole
 * growth field stay inside it. */
_Static_assert(sizeof(VolumeLedger) <= 512, "the Ledger's record is one sector");

/* Blocks in the data run are counted from zero, so zero cannot also mean "a
 * block". Nothing is allocated at data block 0 by anybody: mkfs puts the tag
 * registry there and marks it used before a volume is ever mounted. */
#define VOLUME_LEDGER_NO_BLOCK  0u

#endif /* VOLUME_LEDGER_H */
