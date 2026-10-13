#ifndef TAGFS_PLOT_WIDTH_H
#define TAGFS_PLOT_WIDTH_H

#include "../../../lib/kernel/ktypes.h"

/*
 * The widths format v1 gives a file's record and a tag id, owned here and
 * nowhere else.
 *
 * A record is one packed run of bytes in a metadata pool block: a 42-byte head
 * (40 bytes of fields, then a CRC16), two bytes a tag, six bytes an extent and
 * the name — 42 + 2T + 6E + N — and it never crosses a block, so it is at most
 * the 4080 bytes a pool block carries past its header. An extent is a u32
 * first block and a u16 count, so no extent names more than 0xFFFF blocks, and
 * T, E and N are u16 on the medium too.
 *
 * Memory knows none of these widths: a file's runs are Strips (strip.h), a
 * u64 first block and a u32 count, and its counts are u32 (tagfs.h). What
 * reaches the medium is judged against these by PlotFits and PackV1 (plot.h)
 * alone: PackV1 cuts a Strip longer than an extent into extents of at most
 * PLOT_RUN_MOST blocks — PlotFits counts the record's extents after that cut —
 * and refuses, before anything is written, a record that would not fit, a
 * block past a u32 or a file id past one, rather than cut it.
 */

#define TAGFS_MPOOL_DATA_SIZE   4080    /* a record's ceiling: one pool block's payload */
#define RECORD_HEADER_SIZE      42      /* the record's fields (40) and its CRC16 (2) */
#define RECORD_CRC_OFFSET       40      /* where that CRC16 sits in the record */

/* An extent of a record on the medium — PackV1's and UnpackV1's, and nothing
 * in memory's. */
typedef struct __packed {
    uint32_t start_block;
    uint16_t block_count;
} FileExtent;

STATIC_ASSERT(sizeof(FileExtent) == 6, "FileExtent must be 6 bytes");

/* The most blocks one extent names: its u16 count. */
#define PLOT_RUN_MOST           0xFFFFu

/* The highest tag id format v1 carries. A record's tags and the registry's
 * records hold a tag id in a u16, and a kernel from before tag ids were u32
 * read bit 15 as its Logbook's and dropped any id past this one — so a v1
 * volume's registry issues none past it, and the volume stays readable to
 * every kernel that mounts it. */
#define TAGFS_V1_TAG_ID_MOST    0x7FFEu

#endif /* TAGFS_PLOT_WIDTH_H */
