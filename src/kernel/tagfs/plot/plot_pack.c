#include "plot_internal.h"
#include "../../../lib/kernel/crypto.h"

/*
 * A file's record, as format v1 lays it in a metadata pool block:
 *
 *   u16 record_len | u32 file_id | u32 flags | u64 size | u64 created_time |
 *   u64 modified_time | u16 tag_count | u16 extent_count | u16 name_len |
 *   u16 crc16 | u16 tag_ids[tag_count] | FileExtent extents[extent_count] |
 *   char name[name_len]
 *
 * The CRC16 is taken over the whole record with its own two bytes zeroed. The
 * three counts are u16 on the medium and u32 in memory: a record whose counts
 * or length would not fit is refused here, never cut.
 *
 * ‼ THE ONLY PLACE MEMORY'S RUNS MEET THE WIDTHS OF FORMAT V1. A Strip (strip.h)
 * counts in u32 from a u64 block; an extent counts in u16 from a u32 one. A
 * Strip longer than PLOT_RUN_MOST is cut here into as many extents as it takes,
 * and the record's room is judged on what that cut makes (PackV1Extents,
 * PlotFits) — never on the Strips — so a record that fits in memory fits on
 * the medium. What the cut cannot carry — a block past a u32, a run that owns
 * no ground, a file id past a u32, a tag id past TAGFS_V1_TAG_ID_MOST — is
 * refused, said, before a byte is written. Reading back, neighbouring extents
 * join into one Strip again, and each u16 tag id is a u32 in memory.
 */

#define PLOT_V1_COUNT_MOST 0xFFFFu    /* the u16 counts of the record head */

static uint32_t PlotNameBytes(const TagFSMetadata *meta)
{
    return meta->filename ? (uint32_t)strlen(meta->filename) : 0;
}

uint64_t PackV1Extents(const Strip *strips, uint32_t count)
{
    uint64_t extents = 0;
    for (uint32_t i = 0; i < count; i++)
        extents += ((uint64_t)strips[i].count + PLOT_RUN_MOST - 1u) / PLOT_RUN_MOST;
    return extents;
}

uint64_t PackV1RecordExtents(const FileRecord *rec)
{
    StripWalk walk;
    Strip     run;
    uint64_t  extents = 0;
    StripWalkBegin(rec, &walk);
    while (StripWalkNext(&walk, &run))
        extents += ((uint64_t)run.count + PLOT_RUN_MOST - 1u) / PLOT_RUN_MOST;
    return extents;
}

bool PlotFits(const TagFSMetadata *record, uint32_t *out_bytes)
{
    return PlotFitsExtents(record, PackV1Extents(record->extents, record->extent_count), out_bytes);
}

bool PlotFitsExtents(const TagFSMetadata *record, uint64_t extents, uint32_t *out_bytes)
{
    uint32_t name = PlotNameBytes(record);
    if (record->tag_count > PLOT_V1_COUNT_MOST || extents > PLOT_V1_COUNT_MOST ||
        name > PLOT_V1_COUNT_MOST)
        return false;

    uint64_t bytes = (uint64_t)RECORD_HEADER_SIZE
                   + (uint64_t)record->tag_count * sizeof(uint16_t)
                   + extents * sizeof(FileExtent)
                   + name;
    if (bytes > TAGFS_MPOOL_DATA_SIZE)
        return false;
    if (out_bytes)
        *out_bytes = (uint32_t)bytes;
    return true;
}

uint32_t PlotRunRoom(const TagFSMetadata *record)
{
    uint64_t fixed = (uint64_t)RECORD_HEADER_SIZE + (uint64_t)record->tag_count * sizeof(uint16_t)
                   + PlotNameBytes(record);
    if (fixed >= TAGFS_MPOOL_DATA_SIZE)
        return 0;
    return (uint32_t)((TAGFS_MPOOL_DATA_SIZE - fixed) / sizeof(FileExtent));
}

/* Every extent the record carries names at least one block, none past a u32
 * block number, and none outside a data run of `total` blocks (0: none judged). */
static bool PlotExtentsInRun(const uint8_t *packed, uint32_t count, uint64_t total,
                             uint32_t *out_bad)
{
    for (uint32_t e = 0; e < count; e++) {
        FileExtent x;
        memcpy(&x, packed + (size_t)e * sizeof(FileExtent), sizeof(x));
        uint64_t start = x.start_block;
        uint64_t n     = x.block_count;
        if (n == 0 || start + n > UINT32_MAX || (total && start + n > total)) {
            *out_bad = e;
            return false;
        }
    }
    return true;
}

/* What PackV1 refuses of a record's Strips, before a byte is written: why,
 * and which Strip. */
typedef enum {
    PLOT_STRIPS_CARRIED = 0,
    PLOT_STRIPS_PAST_V1,      /* a block past a u32, or a run that owns no ground */
    PLOT_STRIPS_OUTSIDE,      /* a block outside the data run, or a run of none */
} PlotStripsVerdict;

static PlotStripsVerdict PlotStripsCarried(const Strip *strips, uint32_t count, uint64_t total,
                                           uint32_t *out_bad)
{
    for (uint32_t i = 0; i < count; i++) {
        uint64_t start = strips[i].start;
        uint64_t n     = strips[i].count;
        *out_bad = i;
        if (strips[i].kind != STRIP_DATA || start > UINT32_MAX || start + n > UINT32_MAX)
            return PLOT_STRIPS_PAST_V1;
        if (n == 0 || (total && start + n > total))
            return PLOT_STRIPS_OUTSIDE;
    }
    return PLOT_STRIPS_CARRIED;
}

/* The Strips laid as extents at `out`, each cut into pieces of at most
 * PLOT_RUN_MOST blocks: the bytes laid. */
static uint32_t PlotLayExtents(const Strip *strips, uint32_t count, uint8_t *out)
{
    uint32_t pos = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t start = strips[i].start;
        uint32_t left  = strips[i].count;
        while (left > 0) {
            uint32_t   take = left < PLOT_RUN_MOST ? left : PLOT_RUN_MOST;
            FileExtent x    = { .start_block = (uint32_t)start, .block_count = (uint16_t)take };
            memcpy(out + pos, &x, sizeof(x));
            pos   += sizeof(x);
            start += take;
            left  -= take;
        }
    }
    return pos;
}

/* A file id as the record carries it: OK, or ERR_TAGFS_FID_EXHAUSTED — said —
 * for one past the u32 of format v1, which no v1 Ledger issues. */
static error_t PlotFidCarried(uint64_t fid)
{
    if (fid <= UINT32_MAX)
        return OK;
    kprintf("[TagFS] file %llu: its id is past the 32 bits a record of this volume carries "
            "— the record is not written\n", (unsigned long long)fid);
    return ERR_TAGFS_FID_EXHAUSTED;
}

/* Every tag id the record wears inside what format v1 carries: OK, or
 * ERR_TAGFS_RECORD_FULL — said — for one past it, which no v1 registry issues. */
static error_t PlotTagsCarried(const TagFSMetadata *meta)
{
    for (uint32_t i = 0; meta->tag_ids && i < meta->tag_count; i++) {
        if (meta->tag_ids[i] <= TAGFS_V1_TAG_ID_MOST)
            continue;
        kprintf("[TagFS] file %llu: tag %u of its record is id %u, past the %u a record of this "
                "volume carries — the record is not written\n",
                (unsigned long long)meta->file_id, i, meta->tag_ids[i], TAGFS_V1_TAG_ID_MOST);
        return ERR_TAGFS_RECORD_FULL;
    }
    return OK;
}

error_t PackV1(const TagFSMetadata *meta, uint64_t run_blocks, uint8_t *out, uint32_t *out_bytes)
{
    uint64_t fid     = meta->file_id;
    error_t  carried = PlotFidCarried(fid);
    if (carried == OK)
        carried = PlotTagsCarried(meta);
    if (carried != OK)
        return carried;
    uint32_t bad = 0;
    PlotStripsVerdict verdict = meta->extent_count && !meta->extents
                              ? PLOT_STRIPS_OUTSIDE
                              : PlotStripsCarried(meta->extents, meta->extent_count, run_blocks,
                                                  &bad);
    if (verdict == PLOT_STRIPS_PAST_V1) {
        const Strip *s = &meta->extents[bad];
        kprintf("[TagFS] file %llu: run %u of its record — %u block(s) from %llu%s — is past what "
                "a record of this volume carries (a u32 block, ground and not a hole) — the "
                "record is not written\n", (unsigned long long)fid, bad, s->count,
                (unsigned long long)s->start, s->kind == STRIP_DATA ? "" : ", a hole");
        return ERR_TAGFS_RECORD_FULL;
    }
    if (verdict == PLOT_STRIPS_OUTSIDE) {
        kprintf("[TagFS] file %llu: run %u of its record names no block of the data run — "
                "the record is not written\n", (unsigned long long)fid, bad);
        return ERR_TAGFS_METADATA_ERROR;
    }
    uint32_t bytes   = 0;
    uint64_t extents = PackV1Extents(meta->extents, meta->extent_count);
    if (!PlotFits(meta, &bytes)) {
        kprintf("[TagFS] file %llu: its record would carry %u tag(s), %llu extent(s) and a "
                "%u-byte name, wider than a record of this volume holds (%u bytes) — it is "
                "not written\n", (unsigned long long)fid, meta->tag_count,
                (unsigned long long)extents, PlotNameBytes(meta), TAGFS_MPOOL_DATA_SIZE);
        return ERR_TAGFS_RECORD_FULL;
    }

    uint16_t record_len = (uint16_t)bytes;
    uint32_t file_id    = (uint32_t)fid;
    uint16_t tags       = (uint16_t)meta->tag_count;
    uint16_t extent_n   = (uint16_t)extents;
    uint16_t name       = (uint16_t)PlotNameBytes(meta);
    uint16_t crc        = 0;
    uint32_t pos        = 0;

    memcpy(out + pos, &record_len,          2);  pos += 2;
    memcpy(out + pos, &file_id,             4);  pos += 4;
    memcpy(out + pos, &meta->flags,         4);  pos += 4;
    memcpy(out + pos, &meta->size,          8);  pos += 8;
    memcpy(out + pos, &meta->created_time,  8);  pos += 8;
    memcpy(out + pos, &meta->modified_time, 8);  pos += 8;
    memcpy(out + pos, &tags,                2);  pos += 2;
    memcpy(out + pos, &extent_n,            2);  pos += 2;
    memcpy(out + pos, &name,                2);  pos += 2;
    memcpy(out + pos, &crc,                 2);  pos += 2;
    for (uint32_t i = 0; i < tags; i++) {
        uint16_t tag = (uint16_t)meta->tag_ids[i];
        memcpy(out + pos, &tag, sizeof(tag));
        pos += sizeof(tag);
    }
    pos += PlotLayExtents(meta->extents, meta->extent_count, out + pos);
    if (name)
        memcpy(out + pos, meta->filename, name);

    crc = KCrc16(out, record_len);
    memcpy(out + RECORD_CRC_OFFSET, &crc, 2);
    *out_bytes = record_len;
    return OK;
}

error_t PackV1Refile(uint8_t *rec, uint64_t fid)
{
    error_t carried = PlotFidCarried(fid);
    if (carried != OK)
        return carried;

    uint16_t record_len;
    uint16_t crc     = 0;
    uint32_t file_id = (uint32_t)fid;
    memcpy(&record_len, rec, 2);
    memcpy(rec + 2, &file_id, 4);
    memcpy(rec + RECORD_CRC_OFFSET, &crc, 2);
    crc = KCrc16(rec, record_len);
    memcpy(rec + RECORD_CRC_OFFSET, &crc, 2);
    return OK;
}

/* The record's CRC16 taken as it was stamped — its own two bytes as zeros —
 * in three pieces, so no copy of the record is made to zero them. */
static uint16_t PlotRecordCrc(const uint8_t *rec, uint16_t record_len)
{
    static const uint8_t zeros[2] = { 0, 0 };
    uint16_t crc = KCrc16(rec, RECORD_CRC_OFFSET);
    crc = KCrc16Continue(crc, zeros, sizeof(zeros));
    return KCrc16Continue(crc, rec + RECORD_CRC_OFFSET + 2,
                          (uint32_t)record_len - RECORD_CRC_OFFSET - 2);
}

/* The record's three counts, which sit just before its CRC16 (the layout at
 * the top of this file). */
#define PLOT_V1_COUNTS_OFFSET (RECORD_CRC_OFFSET - 3u * sizeof(uint16_t))

/* Whether the record at `rec` checks out — its length inside `room`, its
 * CRC16, and what its counts say it carries inside its length — and its three
 * counts when it does. Reads only. */
static bool PlotRecordChecks(const uint8_t *rec, uint32_t room, uint16_t counts[3])
{
    uint16_t record_len, stored_crc;

    memcpy(&record_len, rec, 2);
    if (record_len < RECORD_HEADER_SIZE || record_len > room)
        return false;

    /* A stored zero is checked like any other sum: CRC-16/CCITT-FALSE makes
     * 0x0000 for real, and no volume this kernel mounts predates the sum. */
    memcpy(&stored_crc, rec + RECORD_CRC_OFFSET, 2);
    if (PlotRecordCrc(rec, record_len) != stored_crc)
        return false;

    /* What the counts say the record carries lies inside what its length
     * says: a record whose counts reach past its own end is not followed. */
    memcpy(counts, rec + PLOT_V1_COUNTS_OFFSET, 3u * sizeof(uint16_t));
    return (uint32_t)RECORD_HEADER_SIZE + (uint32_t)counts[0] * sizeof(uint16_t) +
           (uint32_t)counts[1] * sizeof(FileExtent) + counts[2] <= record_len;
}

uint64_t UnpackV1Fid(const uint8_t *rec, uint32_t room)
{
    uint16_t counts[3];
    uint32_t fid = 0;
    if (PlotRecordChecks(rec, room, counts))
        memcpy(&fid, rec + 2, sizeof(fid));
    return fid;
}

error_t UnpackV1(const uint8_t *rec, uint32_t room, uint64_t run_blocks, TagFSMetadata *out,
                 bool *out_dropped)
{
    uint16_t counts[3];

    if (out_dropped)
        *out_dropped = false;
    if (!PlotRecordChecks(rec, room, counts))
        return ERR_METADATA_CORRUPTED;
    uint16_t tags = counts[0], extents = counts[1], name = counts[2];

    uint32_t pos = 2;
    uint32_t fid = 0;
    memcpy(&fid,                rec + pos, 4);  pos += 4;
    memcpy(&out->flags,         rec + pos, 4);  pos += 4;
    memcpy(&out->size,          rec + pos, 8);  pos += 8;
    memcpy(&out->created_time,  rec + pos, 8);  pos += 8;
    memcpy(&out->modified_time, rec + pos, 8);
    pos = RECORD_HEADER_SIZE;   /* past the counts and the CRC16, checked above */

    out->file_id      = fid;
    out->tag_count    = tags;
    out->extent_count = 0;
    out->tag_ids      = NULL;
    out->extents      = NULL;
    out->filename     = NULL;

    if (tags) {
        out->tag_ids = kmalloc(sizeof(uint32_t) * tags);
        if (!out->tag_ids)
            return ERR_NO_MEMORY;
        for (uint32_t i = 0; i < tags; i++) {
            uint16_t tag;
            memcpy(&tag, rec + pos, sizeof(tag));
            out->tag_ids[i] = tag;
            pos += sizeof(tag);
        }
    }

    /* An extent outside the data run is not a record to follow: the file is
     * handed out naming no blocks, and PlotLoad refuses to write over a file
     * whose size its blocks do not reach. The caller says so. What follows is
     * joined: an extent that begins where the one before it ends is the same
     * run, cut only because format v1 counts no further. */
    uint32_t bad = 0;
    if (extents && !PlotExtentsInRun(rec + pos, extents, run_blocks, &bad)) {
        if (out_dropped)
            *out_dropped = true;
    } else if (extents) {
        out->extents = kmalloc(sizeof(Strip) * extents);
        if (!out->extents) {
            tagfs_metadata_free(out);
            return ERR_NO_MEMORY;
        }
        for (uint32_t e = 0; e < extents; e++) {
            FileExtent x;
            memcpy(&x, rec + pos + (size_t)e * sizeof(FileExtent), sizeof(x));
            PlotRunsPut(out->extents, &out->extent_count, x.start_block, x.block_count);
        }
    }
    pos += (uint32_t)extents * sizeof(FileExtent);

    out->filename = kmalloc((size_t)name + 1);
    if (!out->filename) {
        tagfs_metadata_free(out);
        return ERR_NO_MEMORY;
    }
    if (name)
        memcpy(out->filename, rec + pos, name);
    out->filename[name] = '\0';
    return OK;
}
