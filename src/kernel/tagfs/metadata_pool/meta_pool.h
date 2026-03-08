#ifndef META_POOL_H
#define META_POOL_H

#include "../tagfs.h"

int      meta_pool_init(uint32_t first_block, uint32_t block_count);
void     meta_pool_shutdown(void);

int      meta_pool_read(uint32_t block, uint32_t offset, TagFSMetadata* out);
int      meta_pool_write(const TagFSMetadata* meta, uint32_t* out_block, uint32_t* out_offset);
int      meta_pool_delete(uint32_t block, uint32_t offset);

void     tagfs_metadata_free(TagFSMetadata* meta);
uint32_t meta_pool_record_size(const TagFSMetadata* meta);
int      meta_pool_flush(void);

#endif // META_POOL_H
