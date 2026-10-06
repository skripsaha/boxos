#ifndef FILE_TABLE_H
#define FILE_TABLE_H

#include "../tagfs.h"

/* Take up the table that starts at `first_block`: its chain, and its rows into
 * the map of live files. `sealed`: the volume's newest Ledger carries a seal,
 * the mount derives every row from the sealed span of the pool, and a block
 * of the chain that does not read as the table ends the chain there instead of
 * refusing the volume (file_table.c, TableBlockRefused).
 *
 * There is no length parameter and there is not going to be one. The Deed used
 * to state file_table_blocks, mkfs wrote a 1 into it and nothing has ever
 * raised it, so the table came up 510 entries long on every volume regardless
 * of how many files were on it. The length is counted off the chain instead. */
int  file_table_init(uint32_t first_block, bool sealed);

/* The mount's census: the chain's blocks, claimed. */
void FileTableClaimGround(void);

#ifdef CONFIG_VOLUME_TESTS
/* Whether a mount would stand on the table chain from `first_block`, walked
 * by the mount's own rules and read only — nothing of the mounted table is
 * touched. For the stand. */
bool FileTableWouldStand(uint32_t first_block, bool sealed);
#endif

/*
 * Let the table go: its memory, and not one byte written. Only the Settle
 * cycle writes the table (file_table_flush, under the Settle's Scribe), and a
 * volume being taken down — a medium that went away, a mount that failed
 * halfway — must not be written from what memory holds, which may describe a
 * volume that is no longer the one under the head.
 */
void file_table_shutdown(void);

int  file_table_lookup(uint32_t file_id, uint32_t* out_block, uint32_t* out_offset);

/* A file named by the table — a new file, or a row the mount corrects — and
 * its block owed to the medium; and a file let go. Under the volume's state
 * lock (an Ink), or with the door shut (the mount). */
int  file_table_update(uint32_t file_id, uint32_t meta_block, uint32_t meta_offset);
int  file_table_delete(uint32_t file_id);

/* Write every block of the table that changed since it last landed, growing
 * the chain onto stamped Spares first. The Settle cycle alone calls it, under
 * its Scribe and holding no TagFS lock; asked any other way it writes nothing
 * and answers ERR_INVALID_STATE. OK when every row changed before the call is
 * on the medium — or waits in memory past a chain the volume had no ground,
 * or the machine no memory, to grow (said once; the pool's records name those
 * rows); else the medium's refusal. */
error_t file_table_flush(void);

#endif // FILE_TABLE_H
