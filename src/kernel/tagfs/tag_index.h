#ifndef TAG_INDEX_H
#define TAG_INDEX_H

#include "ktypes.h"
#include "tagfs.h"

// Hash function for tag strings
uint32_t tag_hash(const char* tag_string);

// Initialize tag index
TagIndex* tag_index_create(void);

// Destroy tag index and free memory
void tag_index_destroy(TagIndex* index);

// Add file to tag index
int tag_index_add_file(TagIndex* index, const char* tag_string, uint32_t file_id);

// Remove file from tag index
int tag_index_remove_file(TagIndex* index, const char* tag_string, uint32_t file_id);

// Query files by tag (returns array of file_ids)
int tag_index_query(TagIndex* index, const char* tag_string, uint32_t** file_ids, uint32_t* count);

// Rebuild entire index from metadata cache
int tag_index_rebuild(TagIndex* index, TagFSMetadata* metadata_cache, uint32_t total_files);

// Clear all entries
void tag_index_clear(TagIndex* index);

#endif // TAG_INDEX_H
