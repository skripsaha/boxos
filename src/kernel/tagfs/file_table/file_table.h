#ifndef FILE_TABLE_H
#define FILE_TABLE_H

#include "../tagfs.h"

/* Take up the table that starts at `first_block`.
 *
 * There is no length parameter and there is not going to be one. The Deed used
 * to state file_table_blocks, mkfs wrote a 1 into it and nothing has ever
 * raised it, so the table came up 510 entries long on every volume regardless
 * of how many files were on it. The length is counted off the chain instead. */
int  file_table_init(uint32_t first_block);

/*
 * Let the table go: its memory, and not one byte written. Only the volume's
 * sync writes the table (file_table_flush, under the sync's Scribe), and a
 * volume being taken down — a medium that went away, a mount that failed
 * halfway — must not be written from what memory holds, which may describe a
 * volume that is no longer the one under the head.
 */
void file_table_shutdown(void);

int  file_table_lookup(uint32_t file_id, uint32_t* out_block, uint32_t* out_offset);
int  file_table_update(uint32_t file_id, uint32_t meta_block, uint32_t meta_offset);
int  file_table_delete(uint32_t file_id);
/* Write every block of the table that changed since it last landed, growing
 * the chain onto stamped Spares first. The volume's sync alone calls it, under
 * its Scribe and holding no TagFS lock; asked any other way it writes nothing
 * and answers -1. 0 when every row changed before the call is on the medium. */
int  file_table_flush(void);

#endif // FILE_TABLE_H
