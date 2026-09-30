#ifndef TAGFS_PLOT_WIDTH_H
#define TAGFS_PLOT_WIDTH_H

#include "../../../lib/kernel/ktypes.h"

/*
 * The widths format v1 gives a file's record, owned here and nowhere else.
 *
 * A record is one packed run of bytes in a metadata pool block: a 42-byte head
 * (40 bytes of fields, then a CRC16), two bytes a tag, six bytes an extent and
 * the name — 42 + 2T + 6E + N — and it never crosses a block, so it is at most
 * the 4080 bytes a pool block carries past its header. An extent is a u32
 * first block and a u16 count, so no extent names more than 0xFFFF blocks, and
 * T, E and N are u16 on the medium too.
 *
 * Memory counts in u32 (tagfs.h); what reaches the medium is judged against
 * these by PlotFits and PackV1 (plot.h), which refuse a record that would not
 * fit rather than cut it. Format v2 changes these widths here and in the
 * packer, and nowhere else.
 */

#define TAGFS_MPOOL_DATA_SIZE   4080    /* a record's ceiling: one pool block's payload */
#define RECORD_HEADER_SIZE      42      /* the record's fields (40) and its CRC16 (2) */
#define RECORD_CRC_OFFSET       40      /* where that CRC16 sits in the record */

typedef struct __packed {
    uint32_t start_block;
    uint16_t block_count;
} FileExtent;

STATIC_ASSERT(sizeof(FileExtent) == 6, "FileExtent must be 6 bytes");

/* The most blocks one extent names: its u16 count. */
#define PLOT_RUN_MOST           0xFFFFu

#endif /* TAGFS_PLOT_WIDTH_H */
