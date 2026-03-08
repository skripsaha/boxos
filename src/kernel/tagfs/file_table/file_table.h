#ifndef FILE_TABLE_H
#define FILE_TABLE_H

#include "../tagfs.h"

int  file_table_init(uint32_t first_block, uint32_t block_count);
void file_table_shutdown(void);

int  file_table_lookup(uint32_t file_id, uint32_t* out_block, uint32_t* out_offset);
int  file_table_update(uint32_t file_id, uint32_t meta_block, uint32_t meta_offset);
int  file_table_delete(uint32_t file_id);
int  file_table_flush(void);

#endif // FILE_TABLE_H
