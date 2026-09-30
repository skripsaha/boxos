#ifndef META_POOL_H
#define META_POOL_H

#include "../tagfs.h"

/* Take up the pool that starts at `first_block`.
 *
 * There is no length parameter and there is not going to be one. The Deed used
 * to state metadata_pool_blocks and this used to believe it — a number written
 * once when the volume was made, never raised by the code that lengthens the
 * chain, and therefore an ever-shorter view of the pool as the volume was
 * used. The length is walked off the medium instead. */
int      meta_pool_init(uint32_t first_block);

/* Let the pool go. `write_back` false means exactly that and nothing else is
 * written: a medium that went away, or a mount that failed halfway and is
 * being taken apart, must not be written from what memory holds, which may
 * describe a volume that is no longer the one under the head. This used to
 * flush unconditionally, so tagfs_abandon(), whose whole contract is "without
 * writing a byte", wrote through here. */
void     meta_pool_shutdown(bool write_back);

/* The record at (block, offset): 0, or a negative -ERR_* — the medium refused
 * the block (ERR_READ_FAILED), the record does not check out
 * (ERR_METADATA_CORRUPTED), or memory ran out for it (ERR_NO_MEMORY). */
int      meta_pool_read(uint32_t block, uint32_t offset, TagFSMetadata* out);
int      meta_pool_write(const TagFSMetadata* meta, uint32_t* out_block, uint32_t* out_offset);

/* Retire the record at (block, offset): its file_id is dropped, in memory. The
 * medium is not touched — see the note over MetaPoolRetire for why, and
 * meta_pool_flush_retires for when. */
int      meta_pool_delete(uint32_t block, uint32_t offset);

/* The newest record of every file the file table names, read into the map of
 * live files (file_map/) — so the table's rows must be in it first. A record
 * of a file no row names is passed over. */
int      meta_pool_mirror_init(void);
/* The file's record, copied out of the map; -1 when the map holds none. */
int      meta_pool_read_cached(uint32_t file_id, TagFSMetadata* out);

/* Where the pool holds this file's newest record. The mirror walk reads the
 * chain in append order, so last-wins is newest by construction — which makes
 * this the answer a mount uses to correct a file table that names somewhere
 * else. -1 when the pool has nothing for this file. */
int      meta_pool_mirror_where(uint32_t file_id, uint32_t* out_block, uint32_t* out_offset);

void     tagfs_metadata_free(TagFSMetadata* meta);

/* The two halves of putting this pool on the medium, and they are two because
 * the file table's flush goes BETWEEN them: a record must exist before anything
 * names it, and the place of the record it replaces must not be destroyed until
 * that name is on the medium. */
int      meta_pool_flush(void);
int      meta_pool_flush_retires(void);

#endif // META_POOL_H
