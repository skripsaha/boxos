#ifndef TAG_BITMAP_H
#define TAG_BITMAP_H

#include "ktypes.h"
#include "tagfs.h"

TagBitmapIndex* tag_bitmap_create(void);
void tag_bitmap_destroy(TagBitmapIndex* index);
void tag_bitmap_clear(TagBitmapIndex* index);

int tag_bitmap_add_file(TagBitmapIndex* index, const char* tag_string, uint32_t file_id);
int tag_bitmap_remove_file(TagBitmapIndex* index, uint32_t file_id);
int tag_bitmap_query(TagBitmapIndex* index, const char* query_tags[],
                     uint32_t tag_count, uint32_t* file_ids, uint32_t max_files);
int tag_bitmap_rebuild(TagBitmapIndex* index, TagFSMetadata* metadata_cache,
                       uint32_t total_files);
bool tag_bitmap_has_tag(TagBitmapIndex* index, const char* tag_string, uint32_t file_id);

uint32_t tag_hash(const char* tag_string);

#endif // TAG_BITMAP_H
