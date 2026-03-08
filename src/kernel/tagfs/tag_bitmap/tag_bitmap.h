#ifndef TAG_BITMAP_H
#define TAG_BITMAP_H

#include "tagfs.h"

TagBitmapIndex* tag_bitmap_create(uint32_t initial_tag_cap, uint32_t initial_file_cap);
void            tag_bitmap_destroy(TagBitmapIndex* idx);
int             tag_bitmap_set(TagBitmapIndex* idx, uint16_t tag_id, uint32_t file_id);
int             tag_bitmap_clear(TagBitmapIndex* idx, uint16_t tag_id, uint32_t file_id);
void            tag_bitmap_remove_file(TagBitmapIndex* idx, uint32_t file_id);
int             tag_bitmap_query(TagBitmapIndex* idx,
                    const uint16_t* tag_ids, uint32_t tag_count,
                    TagKeyGroup** groups, uint32_t group_count,
                    uint32_t* out_file_ids, uint32_t max_results);
int             tag_bitmap_tags_for_file(TagBitmapIndex* idx, uint32_t file_id,
                    uint16_t* out_ids, uint32_t max_ids);
int             tag_bitmap_tag_count_for_file(TagBitmapIndex* idx, uint32_t file_id);

#endif
