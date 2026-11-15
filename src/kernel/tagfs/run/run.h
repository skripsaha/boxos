#ifndef TAGFS_RUN_H
#define TAGFS_RUN_H

#include "../tagfs.h"

/*
 * Blocks of the data run to and from the medium — one, or a run of them in as
 * few commands as the seat takes (step 1.11).
 *
 * A seat says how much it takes in one command (BoardroomSeatRun): a flash
 * drive sixteen of this volume's blocks, a SATA port thirty-two. A file's
 * blocks that lie in one run on the medium go out that many at a time, so ten
 * megabytes on the stick are a hundred and sixty commands and not two thousand
 * five hundred and sixty. What stays a block's own is its digest: each block
 * a reader hands out is checked against the one the book holds for it
 * (integrity/integrity.h), and a writer's blocks carry theirs into the record
 * that names them.
 *
 * TagFSReadRun (tagfs.h) is this door's too. A single block's doors,
 * tagfs_read_block and tagfs_write_block, stay in tagfs.c.
 */

/* The blocks the volume's seat takes in one command: its answer, not a number
 * chosen here, and at least one. */
uint32_t TagFSRunBlocks(void);

/* `count` blocks from `first` on, out of `buffer`, in as few commands as the
 * seat takes: OK, ERR_DEVICE_NOT_READY once the medium left — asked before a
 * command goes out, and of a refusal its departure explains — ERR_WRITE_FAILED
 * when it refused. *out_landed is the blocks from `first` that landed — all of
 * them on OK, on a refusal those of the commands before the refused one and
 * what the medium says the refused one kept (BoardroomWrite) — and every block
 * that landed has its digest recorded and no read-ahead window hands out what
 * it held. A volume assembled in a Braid takes its blocks one at a time. */
error_t  TagFSWriteRun(uint64_t first, uint32_t count, const void *buffer, uint32_t *out_landed);

/*
 * What the files' own bytes cost the medium since the machine started, for
 * the performance dump (system.perf.dump): the writes of files answered — a
 * program's ObjWrite through its WriteJob and the kernel's tagfs_write — the
 * commands their bytes went out in, on either road, and those bytes. The
 * metadata a Settle cycle writes, a span's zeros and a defrag's copy are not
 * a file's bytes written, and are not counted.
 */
typedef struct {
    uint64_t writes;
    uint64_t commands;
    uint64_t bytes;
} TagFSDataCost;

/* A run of `blocks` blocks of a file sent to the medium, counted at its issue
 * — a refused one among them — as the commands the seat takes it in. */
void TagFSDataCostLaid(uint32_t blocks);
/* A write of a file was answered, whatever the answer. */
void TagFSDataCostWrite(void);
void TagFSDataCostTake(TagFSDataCost *out);
/* One line: the cost so far, and what it comes to a write and a megabyte. */
void TagFSDataCostSay(void);

#endif /* TAGFS_RUN_H */
