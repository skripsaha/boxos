#ifndef FILE_TABLE_H
#define FILE_TABLE_H

#include "../tagfs.h"

int  file_table_init(uint32_t first_block, uint32_t block_count);

/*
 * Let the table go.
 *
 * `write_back` is the ordinary case: the machine is stopping and what is in
 * memory belongs on the medium. It is exactly wrong in the other one — a
 * medium that went away, or a mount that failed halfway and is being taken
 * apart — where what is held in memory describes a volume that may no longer
 * be the one under the head. This used to flush unconditionally, so
 * tagfs_teardown(false), whose whole contract is "without writing a byte",
 * wrote two of them through here and through the metadata pool.
 */
void file_table_shutdown(bool write_back);

int  file_table_lookup(uint32_t file_id, uint32_t* out_block, uint32_t* out_offset);
int  file_table_update(uint32_t file_id, uint32_t meta_block, uint32_t meta_offset);
int  file_table_delete(uint32_t file_id);
int  file_table_flush(void);

#endif // FILE_TABLE_H
