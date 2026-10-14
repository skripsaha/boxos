/*
 * create_tagfs_v1.c — the parts of a format-v1 volume: its shape, its Deed,
 * the Ledger, the integrity map, the registry chain, a file's record in the
 * metadata pool, and how much ground all of it asks for (--ground). The order
 * they are written in is create_tagfs_v1_pack.c.
 */

#include "create_tagfs_v1.h"

/* `data_start_block` is where the data run begins, in the volume's blocks; the
 * block number inside it is what a file's metadata carries, so it means the
 * same thing forever no matter what is laid down in front of it. */
int write_block(FILE* disk, uint32_t data_start_block, uint32_t block,
                const void* data) {
    uint64_t vsector = (uint64_t)(data_start_block + block) * TAGFS_BLOCK_SECTORS;
    return write_at_sector(disk, vsector, data, TAGFS_BLOCK_SIZE);
}

/* One bitmap block is 4096 bytes of bits, and a bit is a data block. */
#define BITMAP_BLOCK_COVERS  ((uint32_t)(TAGFS_BLOCK_SIZE * 8))

/*
 * ‼ THE BITMAP IS LAID DOWN FOR THE GROUND THIS VOLUME MAY ONE DAY TAKE,
 * NOT FOR THE GROUND IT IS BORN ON.
 *
 * A volume takes the ground behind it when it is mounted — write this image to
 * a 64 GB stick and the volume grows into the whole stick instead of leaving
 * 63 GB of it dark. Growing means more data blocks, and more data blocks means
 * more bits. But the bitmap lies IN FRONT of the data run: making it bigger
 * later would push `data_block` along, and a block number in a file's metadata
 * has to mean the same thing for the whole life of the volume. So the room is
 * taken at birth, once, and growth never has to move anything.
 *
 * Sixty-four gibibytes is named because that is the size of the media this
 * system is written to. It costs one bit per 4096-byte block — 32 KiB of
 * bitmap per GiB of ceiling, 2 MiB in all — on an image otherwise 90 MiB.
 * A volume BORN bigger than the ceiling still gets a bitmap that covers it:
 * the number below is a floor under the bitmap, not a cap on the volume.
 */
#define GROWTH_CEILING_BLOCKS \
    ((uint32_t)((64ull * 1024 * 1024 * 1024) / TAGFS_BLOCK_SIZE))
#define GROWTH_CEILING_BITMAP_BLOCKS \
    ((GROWTH_CEILING_BLOCKS + BITMAP_BLOCK_COVERS - 1) / BITMAP_BLOCK_COVERS)

/* 0; -1 when the ground is too small to lay a volume on at all; -2 when it is
 * longer than a format-v1 volume can count.
 *
 * ‼ THE SECOND IS A REFUSAL, AND IT USED TO BE A CUT. A format-v1 volume counts
 * its blocks in 32 bits — LAYOUT, the bitmap, every extent — so it reaches
 * 16 TiB and no further, and a longer ground was cut to the low 32 bits of its
 * block count right here, in silence: the volume was laid out over whatever
 * that remainder came to, and the rest of the ground was never spoken of. */
int compute_layout(uint64_t ground_sectors, VolumeShape* out) {
    memset(out, 0, sizeof(*out));

    if (ground_sectors / TAGFS_BLOCK_SECTORS > UINT32_MAX) return -2;
    out->volume_blocks    = (uint32_t)(ground_sectors / TAGFS_BLOCK_SECTORS);
    out->deed_block       = 0;
    out->ledger_block     = DEED_BLOCKS;
    out->disk_book_block  = out->ledger_block + VOLUME_LEDGER_COPIES;
    out->disk_book_blocks = 2 + DISK_BOOK_ENTRY_BLOCKS;  /* head, backup, records */
    out->bitmap_block     = out->disk_book_block + out->disk_book_blocks;

    /*
     * The bitmap covers the data run and lies in front of it, so its size and
     * the run's size define each other. It starts at the growth ceiling and
     * only ever GROWS from there.
     *
     * ‼ ONLY EVER GROWS, AND THAT IS WHAT MAKES IT SETTLE.
     *
     * A loop that could also shrink can go round in a two-cycle: N blocks
     * leave a run that needs N+1, and N+1 blocks take a block off the front so
     * the run needs only N again. It then runs out of passes and leaves
     * whichever of the two it was holding — and half the time that is the
     * smaller one, which is a volume whose data run is a block wider than its
     * own bitmap can account for. Growing only cannot cycle: a bigger bitmap
     * makes a shorter run, and a shorter run can only ask for less.
     */
    uint32_t bitmap_blocks = GROWTH_CEILING_BITMAP_BLOCKS;
    for (int i = 0; i < 32; i++) {
        uint32_t data_block = out->bitmap_block + bitmap_blocks;
        /* ‼ Checked, because it can now happen. With a two-megabyte bitmap the
         * front matter is 645 blocks, and a ground smaller than that used to
         * make this subtraction wrap to four billion and be believed. */
        if (out->volume_blocks <= data_block + DEED_BLOCKS) return -1;
        uint32_t data_blocks = out->volume_blocks - data_block - DEED_BLOCKS;
        uint32_t need = ((data_blocks + 7) / 8 + TAGFS_BLOCK_SIZE - 1) / TAGFS_BLOCK_SIZE;
        if (need <= bitmap_blocks) break;
        bitmap_blocks = need;
    }

    out->bitmap_blocks = bitmap_blocks;
    out->data_block    = out->bitmap_block + bitmap_blocks;
    if (out->volume_blocks <= out->data_block + DEED_BLOCKS) return -1;
    out->data_blocks   = out->volume_blocks - out->data_block - DEED_BLOCKS;

    /* Said as a fact rather than trusted from the loop above: a volume whose
     * bitmap cannot hold a bit for every block of its data run is a volume
     * that cannot keep track of its own ground, and the kernel refuses to
     * mount one. It must not be possible to MAKE one. */
    if ((uint64_t)out->data_blocks >
        (uint64_t)out->bitmap_blocks * BITMAP_BLOCK_COVERS) return -1;

    /* The far copy goes in the last block of the volume — as far from the head
     * as the ground allows, which is the whole point of having it. */
    out->tail_sector = (uint64_t)(out->volume_blocks - DEED_BLOCKS) * TAGFS_BLOCK_SECTORS;
    return 0;
}

/* ====================================================================
 * Writing the Deed
 *
 * Two copies of one record, at opposite ends of the volume. Both are built
 * here from the same numbers, so the only thing that differs between them is
 * the one byte that says which is which — and a head read where a tail should
 * be is then a misdirected read that says so, instead of a valid-looking Deed.
 * ==================================================================== */

int write_deed(FILE* disk, uint32_t role, uint64_t vsector,
               const uint8_t uuid[16], const VolumeShape* shape,
               const VolumeBoot* boot, uint32_t mpool_blocks,
               uint32_t ftable_blocks, uint32_t registry_blocks,
               const VolumePacked* packed) {
    uint8_t block[TAGFS_BLOCK_SIZE];
    memset(block, 0, sizeof(block));

    VolumeGeometry geo = DeedGeometry();

    VolumeLayout layout;
    memset(&layout, 0, sizeof(layout));
    layout.total_blocks         = shape->volume_blocks;
    layout.state_block          = shape->ledger_block;
    layout.state_blocks         = VOLUME_LEDGER_COPIES;
    layout.tag_registry_block   = shape->data_block + 0;
    layout.file_table_block     = shape->data_block + 1;
    layout.metadata_pool_block  = shape->data_block + 2;
    /* ‼ HOW LONG THESE THREE CHAINS ARE IS STATED AND NOT BELIEVED.
     *
     * A Deed is written once and read for the rest of the volume's life, and
     * all three of these chains lengthen with ordinary use. file_table_blocks
     * was a hard-coded 1 here, so a volume that grew past 510 files lost every
     * entry above that on its next mount; metadata_pool_blocks was right the
     * day it was written and short of the truth ever after, and the kernel
     * walking only that far severed the rest of the pool on every boot.
     * tag_registry_blocks was a hard-coded 1 as well and is now the truth at
     * birth, on the same terms as the other two.
     *
     * The kernel walks all three chains to their end and reads none of these
     * fields. They are still written, truthfully, because the layout stamp is
     * shared with anything else that reads a volume — and a number that is
     * right when the volume is born is worth more than a zero. */
    layout.tag_registry_blocks  = registry_blocks;
    layout.file_table_blocks    = ftable_blocks;
    layout.metadata_pool_blocks = mpool_blocks;
    layout.block_bitmap_block   = shape->bitmap_block;
    layout.block_bitmap_blocks  = shape->bitmap_blocks;
    layout.disk_book_block      = shape->disk_book_block;
    layout.disk_book_blocks     = shape->disk_book_blocks;
    layout.data_block           = shape->data_block;
    layout.data_blocks          = shape->data_blocks;

    VolumeBorn born;
    memset(&born, 0, sizeof(born));
    born.created_unix = (uint64_t)time(NULL);
    memcpy(born.maker, "create_tagfs", 12);

    /* The pen zeroes the block, lays the magic and the prologue, and knows
     * where each stamp goes. Nothing about the FORMAT is spelled here any
     * more — only which stamps this volume is born with and what they say. */
    DeedPen pen;
    DeedPenOpen(&pen, block, sizeof(block));
    if (DeedPenSay(&pen, VOLUME_STAMP_GEOMETRY, &geo,    sizeof(geo))    != 0 ||
        DeedPenSay(&pen, VOLUME_STAMP_LAYOUT,   &layout, sizeof(layout)) != 0 ||
        DeedPenSay(&pen, VOLUME_STAMP_BORN,     &born,   sizeof(born))   != 0 ||
        DeedPenSay(&pen, VOLUME_STAMP_BOOT,     boot,    sizeof(*boot))  != 0 ||
        DeedPenSay(&pen, VOLUME_STAMP_PACKED,   packed,  sizeof(*packed)) != 0) {
        fprintf(stderr, "The stamps of this Deed do not fit the block it lives in\n");
        return -1;
    }

    DeedPenSetUuid(&pen, uuid);
    DeedPenSetGround(&pen,
                     (uint64_t)shape->volume_blocks * TAGFS_BLOCK_SECTORS,
                     shape->tail_sector);
    DeedPenSetRole(&pen, role);
    DeedPenSeal(&pen);

    return write_at_sector(disk, vsector, block, sizeof(block));
}

/* ====================================================================
 * Writing the Ledger
 *
 * Both copies, identical, both valid: a volume that has never been written to
 * still has to mount, and a mount reads whichever copy is newer. They carry
 * the same seq here, and the first write the kernel makes goes to the other
 * one with seq+1 — from then on the pair alternates forever.
 *
 * Both are SEALED (volume_ledger.h, SettleMark): mkfs is the volume's first
 * settled cycle, and a machine stopping cleanly — the pool from its head to
 * its last record and the registry to its last record, with their CRC32s — so
 * the first mount stands on a seal like every mount after it.
 * ==================================================================== */

/* A closed pool block's part in the seal's fold (volume_ledger.h,
 * closed_crc32): the CRC32 of its records, summed again with the block's
 * number and the records' length — metadata_pool/meta_pool.c PoolBlockMix,
 * the same twelve bytes, little-endian. */
uint32_t pool_block_mix(uint32_t block, const MetaPoolBlock* blk) {
    uint32_t mix[3] = { block, blk->used_bytes, tagfs_crc32(blk->payload, blk->used_bytes) };
    return tagfs_crc32((const uint8_t*)mix, sizeof(mix));
}

int write_ledger(FILE* disk, const VolumeShape* shape,
                 const VolumeLedger* src) {
    static const uint8_t magic[8] = {
        VOLUME_LEDGER_MAGIC_0, VOLUME_LEDGER_MAGIC_1, VOLUME_LEDGER_MAGIC_2,
        VOLUME_LEDGER_MAGIC_3, VOLUME_LEDGER_MAGIC_4, VOLUME_LEDGER_MAGIC_5,
        VOLUME_LEDGER_MAGIC_6, VOLUME_LEDGER_MAGIC_7
    };

    VolumeLedger led = *src;
    memcpy(led.magic, magic, sizeof(magic));
    led.bytes = sizeof(VolumeLedger);
    led.crc32 = 0;
    led.crc32 = tagfs_crc32((const uint8_t*)&led, sizeof(led));

    for (uint32_t copy = 0; copy < VOLUME_LEDGER_COPIES; copy++) {
        uint8_t block[TAGFS_BLOCK_SIZE];
        memset(block, 0, sizeof(block));
        memcpy(block, &led, sizeof(led));

        uint64_t vsector = (uint64_t)(shape->ledger_block + copy) * TAGFS_BLOCK_SECTORS;
        if (write_at_sector(disk, vsector, block, sizeof(block)) != 0) return -1;
    }
    return 0;
}

/* ====================================================================
 * The integrity map
 *
 * One 64-bit digest per block of the data run (tagfs/integrity/), a zero
 * digest meaning "unknown", laid down here already zeroed: a mount never lays
 * a map, so it never writes and flushes a run of zeros the size of the volume
 * before the volume is up (RT-15). The Ledger names it (write_ledger's
 * caller).
 * ==================================================================== */

int write_integrity_map(FILE* disk, uint32_t data_start_block, uint32_t first,
                        uint32_t count, uint32_t total_blocks) {
    if (first + count > total_blocks) {
        fprintf(stderr, "Not enough blocks for the integrity map (%u at %u)\n", count, first);
        return -1;
    }
    uint8_t zero[TAGFS_BLOCK_SIZE];
    memset(zero, 0, sizeof(zero));
    for (uint32_t i = 0; i < count; i++) {
        if (write_block(disk, data_start_block, first + i, zero) != 0) {
            fprintf(stderr, "Failed to write integrity map block %u\n", first + i);
            return -1;
        }
    }
    printf("  Integrity map: %u block(s) at data block %u\n", count, first);
    return 0;
}

/* ====================================================================
 * Build tag registry block
 * ==================================================================== */

/* How many bytes one tag takes in a registry block. One expression, used by
 * the writer below and by the ground estimate, so the two cannot drift. */
static uint32_t registry_record_size(const TagEntry* t) {
    uint32_t key_len   = (uint32_t)strlen(t->key);
    uint32_t value_len = t->value ? (uint32_t)strlen(t->value) : 0;
    return 2 + 1 + 1 + 2 + key_len + value_len;   /* tag_id, flags, key_len, value_len */
}

/*
 * ‼ THE REGISTRY CHAINS, THE SAME WAY THE POOL AND THE TABLE ALREADY DO.
 *
 * It used to be one block, and a vocabulary that outgrew it did not chain — it
 * printed "Tag registry block overflow" and the build stopped. Measured at 416
 * tags. Every tag id on a volume is defined in this record, so that was a hard
 * ceiling on how much a BoxOS image is allowed to be ABOUT, discovered by the
 * build breaking rather than by anything saying so.
 *
 * TagRegistryBlock has carried a next_block field the whole time and the kernel
 * follows it; nothing ever wrote one. The continuation blocks come out of the
 * same high-water mark the pool's and the table's do, so the bitmap written
 * later marks them used.
 */
int write_registry_chain(FILE* disk, uint32_t data_start_block,
                         uint32_t* next_free, uint32_t total_blocks,
                         uint32_t* out_blocks, SettleMark* mark) {
    TagRegistryBlock blk;
    memset(&blk, 0, sizeof(blk));
    blk.magic = TAGFS_REGISTRY_MAGIC;

    uint32_t here   = 0;   /* data block 0 — where the Deed says the registry begins */
    uint32_t blocks = 1;
    uint32_t offset = 0;

    for (uint32_t i = 0; i < g_tag_count; i++) {
        uint8_t  key_len   = (uint8_t)strlen(g_tags[i].key);
        uint16_t value_len = g_tags[i].value ? (uint16_t)strlen(g_tags[i].value) : 0;
        uint8_t  flags     = g_tags[i].value ? 1 : 0;

        uint32_t record_size = registry_record_size(&g_tags[i]);

        /* A record is never split across blocks — the kernel's loader walks a
         * block's data[] and stops at its end — so one that cannot fit in an
         * empty block cannot be stored at all, and chaining will not save it. */
        if (record_size > TAGFS_REGISTRY_DATA_SIZE) {
            fprintf(stderr, "The tag '%s' needs %u bytes and a registry block "
                            "holds %u — it cannot be written\n",
                    g_tags[i].key, record_size, TAGFS_REGISTRY_DATA_SIZE);
            return -1;
        }

        if (offset + record_size > TAGFS_REGISTRY_DATA_SIZE) {
            uint32_t next = (*next_free)++;
            if (next >= total_blocks) {
                fprintf(stderr, "Not enough blocks for the tag registry chain\n");
                return -1;
            }

            blk.next_block = next;
            if (write_block(disk, data_start_block, here, &blk) != 0) {
                fprintf(stderr, "Failed to write tag registry block %u\n", here);
                return -1;
            }

            here = next;
            memset(&blk, 0, sizeof(blk));
            blk.magic = TAGFS_REGISTRY_MAGIC;
            offset = 0;
            blocks++;
        }

        uint8_t* p = blk.data + offset;

        /* tag_id (uint16_t) — the kernel RESTORES the tag at this exact id on
         * load (tag_registry.c RegistryAdopt), so the intern order
         * above IS the on-disk ID contract, not a throwaway field. The cargo
         * gives none past TAGFS_V1_TAG_ID_MOST, and no key past the u8. */
        uint16_t tag_id = (uint16_t)g_tags[i].tag_id;
        memcpy(p, &tag_id, 2); p += 2;
        /* flags (uint8_t) */
        *p++ = flags;
        /* key_len (uint8_t) */
        *p++ = key_len;
        /* value_len (uint16_t) */
        memcpy(p, &value_len, 2); p += 2;
        /* key */
        memcpy(p, g_tags[i].key, key_len); p += key_len;
        /* value */
        if (value_len > 0) {
            memcpy(p, g_tags[i].value, value_len);
        }

        offset += record_size;
        blk.entry_count++;
        blk.used_bytes = (uint16_t)offset;
    }

    if (write_block(disk, data_start_block, here, &blk) != 0) {
        fprintf(stderr, "Failed to write tag registry block %u\n", here);
        return -1;
    }

    /* The seal's part of the registry: its last block, and its records. */
    mark->registry_block = here;
    mark->registry_bytes = blk.used_bytes;
    mark->registry_crc32 = tagfs_crc32(blk.data, blk.used_bytes);

    *out_blocks = blocks;
    return 0;
}

/* CRC-16/CCITT-FALSE — must match kernel's meta_crc16 exactly */
static uint16_t meta_crc16(const uint8_t* data, uint32_t len) {
    uint16_t crc = 0xFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
        }
    }
    return crc;
}

/* ====================================================================
 * Build metadata pool record (matches kernel's pack_record exactly)
 * ==================================================================== */

/*
 * ‼ A FILE IS LAID IN ONE RUN AND NAMED BY AS MANY EXTENTS AS THAT RUN NEEDS.
 *
 * An extent counts its blocks in 16 bits, and this used to write the whole
 * file as one extent with its block count cast to them: a file of more than
 * 0xFFFF blocks (256 MiB) went onto the volume named by an extent that held
 * the count modulo 65536 — the kernel read a file cut short and the rest of
 * its blocks stood taken and owned by nothing. The run is now cut into extents
 * of at most 0xFFFF blocks, the most one can count. A record longer than the
 * pool holds is answered with its length and not packed; the caller refuses
 * it.
 */
#define EXTENT_MAX_BLOCKS 0xFFFFu

static uint32_t V1Extents(const FileInfo* fi) {
    return (fi->block_count + EXTENT_MAX_BLOCKS - 1) / EXTENT_MAX_BLOCKS;
}

/* The bytes a file's record takes: its head, two a tag, six an extent and
 * its name — one expression, for the record packed below and for the one
 * judged before the volume's first write (V1PackPlace), so the two cannot
 * come to disagree. */
uint64_t V1RecordBytes(const FileInfo* fi) {
    return RECORD_HEADER_SIZE + 2u * (uint64_t)fi->tag_count +
           (uint64_t)V1Extents(fi) * sizeof(FileExtent) + strlen(fi->filename);
}

uint64_t pack_metadata_record(uint8_t* buf, const FileInfo* fi) {
    const char* fname = fi->filename;
    uint16_t name_len = (uint16_t)strlen(fname);
    uint32_t extents  = V1Extents(fi);

    uint64_t record_size = V1RecordBytes(fi);
    if (record_size > TAGFS_MPOOL_DATA_SIZE)
        return record_size;
    uint16_t record_len   = (uint16_t)record_size;
    uint16_t extent_count = (uint16_t)extents;
    uint16_t tag_count    = (uint16_t)fi->tag_count;

    uint32_t pos = 0;

    /* record_len */
    memcpy(buf + pos, &record_len, 2);            pos += 2;
    /* file_id */
    memcpy(buf + pos, &fi->file_id, 4);           pos += 4;
    /* flags */
    uint32_t flags = TAGFS_FILE_ACTIVE;
    memcpy(buf + pos, &flags, 4);                 pos += 4;
    /* size */
    memcpy(buf + pos, &fi->file_size, 8);         pos += 8;
    /* created_time */
    uint64_t now = (uint64_t)time(NULL);
    memcpy(buf + pos, &now, 8);                   pos += 8;
    /* modified_time */
    memcpy(buf + pos, &now, 8);                   pos += 8;
    /* tag_count */
    memcpy(buf + pos, &tag_count, 2);             pos += 2;
    /* extent_count */
    memcpy(buf + pos, &extent_count, 2);          pos += 2;
    /* name_len */
    memcpy(buf + pos, &name_len, 2);              pos += 2;
    /* CRC16 placeholder — zeroed for computation, stamped below */
    uint16_t zero_crc = 0;
    memcpy(buf + pos, &zero_crc, 2);              pos += 2;

    /* tag_ids[] — each a u16: the cargo gives none past TAGFS_V1_TAG_ID_MOST */
    for (uint32_t t = 0; t < fi->tag_count; t++) {
        uint16_t tag_id = (uint16_t)fi->tag_ids[t];
        memcpy(buf + pos, &tag_id, 2);            pos += 2;
    }

    /* extents[] — the file's one run, cut at the width of an extent */
    for (uint32_t e = 0; e < extents; e++) {
        uint32_t   left = fi->block_count - e * EXTENT_MAX_BLOCKS;
        FileExtent ext;
        ext.start_block = fi->start_block + e * EXTENT_MAX_BLOCKS;
        ext.block_count = (uint16_t)(left < EXTENT_MAX_BLOCKS ? left : EXTENT_MAX_BLOCKS);
        memcpy(buf + pos, &ext, sizeof(FileExtent));
        pos += sizeof(FileExtent);
    }

    /* filename */
    memcpy(buf + pos, fname, name_len);
    pos += name_len;

    /* Stamp CRC16 over the entire record (with CRC field zeroed) */
    uint16_t crc = meta_crc16(buf, (uint32_t)record_len);
    memcpy(buf + RECORD_CRC_OFFSET, &crc, 2);

    return record_len;
}

/* ====================================================================
 *  --ground: how far the volume's ground must run to hold these files
 *
 * ‼ THE SIZE OF A VOLUME IS NOT A NUMBER ANYBODY TYPES.
 *
 * It was: the build made a 24 MiB region because 51200 was written in the
 * Makefile, and the day the files inside came to more than that, the build did
 * not say "the medium is too small" — it made the volume anyway, with four
 * hundred kilobytes left, and the C++ suite failed seventy checks because it
 * had nowhere to write its test files. A ceiling that is discovered by what
 * breaks under it is not a ceiling, it is a trap.
 *
 * So the tool that lays a volume out is asked first how much ground it needs,
 * and the answer is what the medium is then made to give. Adding a program to
 * the image grows the image. Nothing has to be raised by hand, ever.
 *
 * The answer is the CONTENT plus the ROOM A RUNNING MACHINE WORKS IN — logs
 * poured by logsave, drafts, CoW snapshots, the files a test suite makes and
 * deletes. That room is a policy, stated once, here, with its reason: a
 * machine that cannot write is a machine that cannot be used, and the cost of
 * being generous is sixty-four mebibytes on a medium that has gigabytes.
 * ==================================================================== */

/* Sixty-four mebibytes of room, in the volume's own blocks. */
#define GROUND_WORKING_ROOM_BLOCKS   ((64u * 1024u * 1024u) / TAGFS_BLOCK_SIZE)

/* The metadata pool chains into the data run as records outgrow one block. The
 * image's own record count settles in two blocks today; eight is the room for
 * that to double twice without anybody noticing it had a bound. */
#define GROUND_METADATA_SLACK_BLOCKS 8u

int say_the_ground(int pair_count, char* argv[], int first) {
    uint64_t data_blocks = 3;   /* registry, file table, metadata pool */
    data_blocks += GROUND_METADATA_SLACK_BLOCKS;

    /* The file table chains too, now that it is allowed to be longer than 510
     * entries. File ids run 1..pair_count, so the table is pair_count+1 entries
     * and everything past the first block is ground this has to ask for. */
    data_blocks += ((uint64_t)pair_count + 1u + TAGFS_FTABLE_PER_BLOCK - 1u)
                   / TAGFS_FTABLE_PER_BLOCK;

    for (int i = 0; i < pair_count; i++) {
        const char* path = argv[first + i * 2];
        FILE* f = fopen(path, "rb");
        if (!f) {
            fprintf(stderr, "create_tagfs --ground: cannot read %s\n", path);
            return 1;
        }
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fclose(f);
        if (size < 0) size = 0;
        data_blocks += ((uint64_t)size + TAGFS_BLOCK_SIZE - 1) / TAGFS_BLOCK_SIZE;
    }

    /* And the registry, now that it chains as well. It is asked for by
     * interning the very tags the build is about to write, through the same
     * routines that write them — a second estimate of how big a vocabulary is
     * would be a second thing to keep true. A record is never split, so a
     * block is closed as soon as the next record will not fit, and that is
     * counted here exactly as write_registry_chain counts it. */
    seed_reserved_tags(TAGFS_V1_TAG_ID_MOST);
    for (int i = 0; i < pair_count; i++) {
        uint32_t* ids   = NULL;
        uint32_t  count = 0;
        if (parse_tags(argv[first + i * 2 + 1], argv[first + i * 2], &ids, &count) != 0) return 1;
        free(ids);
    }
    {
        uint64_t reg_blocks = 1;
        uint32_t offset     = 0;
        for (uint32_t i = 0; i < g_tag_count; i++) {
            uint32_t record_size = registry_record_size(&g_tags[i]);
            if (offset + record_size > TAGFS_REGISTRY_DATA_SIZE) {
                reg_blocks++;
                offset = 0;
            }
            offset += record_size;
        }
        data_blocks += reg_blocks - 1;   /* block 0 is already in the 3 above */
    }

    data_blocks += GROUND_WORKING_ROOM_BLOCKS;

    /* And the integrity map mkfs lays down zeroed (write_integrity_map): one
     * digest per block of the run, a block of digests past the estimate for
     * the run the grain rounds up to. */
    data_blocks += (data_blocks + INTEGRITY_DIGESTS_PER_BLOCK - 1) / INTEGRITY_DIGESTS_PER_BLOCK + 1;

    /* compute_layout answers the other way round — ground in, data run out —
     * and the bitmap in between makes it not quite invertible. So the ground
     * is grown a grain at a time until the run it yields is big enough. It
     * settles in one or two steps from this start and cannot loop: each step
     * adds ground, and the run grows with it. */
    uint64_t ground = (data_blocks + 64) * TAGFS_BLOCK_SECTORS;
    ground = ((ground + GROUND_GRAIN_SECTORS - 1) / GROUND_GRAIN_SECTORS) * GROUND_GRAIN_SECTORS;

    for (;;) {
        VolumeShape shape;
        int laid = compute_layout(ground, &shape);
        if (laid == 0 && shape.data_blocks >= data_blocks)
            break;
        if (laid == -2) {
            fprintf(stderr, "create_tagfs --ground: these files need more than the "
                            "16 TiB a format-v1 volume can count\n");
            return 1;
        }
        ground += GROUND_GRAIN_SECTORS;
    }

    printf("%llu\n", (unsigned long long)ground);
    return 0;
}
