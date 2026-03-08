#ifndef TAGFS_CONTEXT_H
#define TAGFS_CONTEXT_H

#include "../tagfs.h"

void     tagfs_context_init(void);
int      tagfs_context_add_tag(uint32_t pid, uint16_t tag_id);
int      tagfs_context_add_tag_string(uint32_t pid, const char* key, const char* value);
int      tagfs_context_remove_tag(uint32_t pid, uint16_t tag_id);
void     tagfs_context_clear(uint32_t pid);
bool     tagfs_context_matches_file(uint32_t pid, uint32_t file_id);
void     tagfs_context_destroy(uint32_t pid);
uint64_t tagfs_context_get_bits(uint32_t pid);
int      tagfs_context_get_tags(uint32_t pid, const char* tags[], uint32_t max_tags);

#endif
