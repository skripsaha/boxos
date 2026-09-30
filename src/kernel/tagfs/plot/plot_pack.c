#include "plot.h"
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
 */

#define PLOT_V1_COUNT_MOST 0xFFFFu    /* the u16 counts of the record head */

static uint32_t PlotNameBytes(const TagFSMetadata *meta)
{
    return meta->filename ? (uint32_t)strlen(meta->filename) : 0;
}

bool PlotFits(const TagFSMetadata *record, uint32_t *out_bytes)
{
    uint32_t name = PlotNameBytes(record);
    if (record->tag_count > PLOT_V1_COUNT_MOST || record->extent_count > PLOT_V1_COUNT_MOST ||
        name > PLOT_V1_COUNT_MOST)
        return false;

    uint64_t bytes = (uint64_t)RECORD_HEADER_SIZE
                   + (uint64_t)record->tag_count * sizeof(uint16_t)
                   + (uint64_t)record->extent_count * sizeof(FileExtent)
                   + name;
    if (bytes > TAGFS_MPOOL_DATA_SIZE)
        return false;
    if (out_bytes)
        *out_bytes = (uint32_t)bytes;
    return true;
}

/* Every extent names at least one block, none past a u32 block number, and —
 * once the volume is up — none outside the data run. */
static bool PlotExtentsInRun(const FileExtent *extents, uint32_t count, uint32_t *out_bad)
{
    TagFSState *fs    = tagfs_get_state();
    uint64_t    total = (fs && fs->initialized) ? fs->layout.data_blocks : 0;

    for (uint32_t e = 0; e < count; e++) {
        uint64_t start = extents[e].start_block;
        uint64_t n     = extents[e].block_count;
        if (n == 0 || start + n > UINT32_MAX || (total && start + n > total)) {
            *out_bad = e;
            return false;
        }
    }
    return true;
}

uint32_t PackV1(const TagFSMetadata *meta, uint8_t *out)
{
    uint32_t bytes = 0;
    if (!PlotFits(meta, &bytes)) {
        kprintf("[TagFS] file %u: its record would carry %u tag(s), %u extent(s) and a "
                "%u-byte name, wider than a record of this volume holds (%u bytes) — it is "
                "not written\n", meta->file_id, meta->tag_count, meta->extent_count,
                PlotNameBytes(meta), TAGFS_MPOOL_DATA_SIZE);
        return 0;
    }
    uint32_t bad = 0;
    if (meta->extent_count && (!meta->extents ||
                               !PlotExtentsInRun(meta->extents, meta->extent_count, &bad))) {
        kprintf("[TagFS] file %u: extent %u of its record names no block of the data run — "
                "the record is not written\n", meta->file_id, bad);
        return 0;
    }

    uint16_t record_len = (uint16_t)bytes;
    uint16_t tags       = (uint16_t)meta->tag_count;
    uint16_t extents    = (uint16_t)meta->extent_count;
    uint16_t name       = (uint16_t)PlotNameBytes(meta);
    uint16_t crc        = 0;
    uint32_t pos        = 0;

    memcpy(out + pos, &record_len,          2);  pos += 2;
    memcpy(out + pos, &meta->file_id,       4);  pos += 4;
    memcpy(out + pos, &meta->flags,         4);  pos += 4;
    memcpy(out + pos, &meta->size,          8);  pos += 8;
    memcpy(out + pos, &meta->created_time,  8);  pos += 8;
    memcpy(out + pos, &meta->modified_time, 8);  pos += 8;
    memcpy(out + pos, &tags,                2);  pos += 2;
    memcpy(out + pos, &extents,             2);  pos += 2;
    memcpy(out + pos, &name,                2);  pos += 2;
    memcpy(out + pos, &crc,                 2);  pos += 2;
    if (tags) {
        memcpy(out + pos, meta->tag_ids, (uint32_t)tags * sizeof(uint16_t));
        pos += (uint32_t)tags * sizeof(uint16_t);
    }
    if (extents) {
        memcpy(out + pos, meta->extents, (uint32_t)extents * sizeof(FileExtent));
        pos += (uint32_t)extents * sizeof(FileExtent);
    }
    if (name)
        memcpy(out + pos, meta->filename, name);

    crc = KCrc16(out, record_len);
    memcpy(out + RECORD_CRC_OFFSET, &crc, 2);
    return record_len;
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

error_t UnpackV1(const uint8_t *rec, uint32_t room, TagFSMetadata *out)
{
    uint16_t record_len, tags, extents, name, stored_crc;

    memcpy(&record_len, rec, 2);
    if (record_len < RECORD_HEADER_SIZE || record_len > room)
        return ERR_METADATA_CORRUPTED;

    /* A stored zero is checked like any other sum: CRC-16/CCITT-FALSE makes
     * 0x0000 for real, and no volume this kernel mounts predates the sum. */
    memcpy(&stored_crc, rec + RECORD_CRC_OFFSET, 2);
    if (PlotRecordCrc(rec, record_len) != stored_crc)
        return ERR_METADATA_CORRUPTED;

    uint32_t pos = 2;
    memcpy(&out->file_id,       rec + pos, 4);  pos += 4;
    memcpy(&out->flags,         rec + pos, 4);  pos += 4;
    memcpy(&out->size,          rec + pos, 8);  pos += 8;
    memcpy(&out->created_time,  rec + pos, 8);  pos += 8;
    memcpy(&out->modified_time, rec + pos, 8);  pos += 8;
    memcpy(&tags,               rec + pos, 2);  pos += 2;
    memcpy(&extents,            rec + pos, 2);  pos += 2;
    memcpy(&name,               rec + pos, 2);  pos += 2;
    pos += 2;   /* the CRC16, checked above */

    /* What the counts say the record carries lies inside what its length
     * says: a record whose counts reach past its own end is not followed. */
    if ((uint32_t)RECORD_HEADER_SIZE + (uint32_t)tags * sizeof(uint16_t) +
        (uint32_t)extents * sizeof(FileExtent) + name > record_len)
        return ERR_METADATA_CORRUPTED;

    out->tag_count    = tags;
    out->extent_count = extents;
    out->tag_ids      = NULL;
    out->extents      = NULL;
    out->filename     = NULL;

    if (tags) {
        out->tag_ids = kmalloc(sizeof(uint16_t) * tags);
        if (!out->tag_ids)
            return ERR_NO_MEMORY;
        memcpy(out->tag_ids, rec + pos, sizeof(uint16_t) * tags);
        pos += (uint32_t)tags * sizeof(uint16_t);
    }

    if (extents) {
        out->extents = kmalloc(sizeof(FileExtent) * extents);
        if (!out->extents) {
            tagfs_metadata_free(out);
            return ERR_NO_MEMORY;
        }
        memcpy(out->extents, rec + pos, sizeof(FileExtent) * extents);
        pos += (uint32_t)extents * sizeof(FileExtent);

        /* An extent outside the data run is not a record to follow: the file
         * is handed out naming no blocks, and PlotLoad refuses to write over
         * a file whose size its blocks do not reach. */
        uint32_t bad = 0;
        if (!PlotExtentsInRun(out->extents, extents, &bad)) {
            kfree(out->extents);
            out->extents      = NULL;
            out->extent_count = 0;
        }
    }

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
