#ifndef TAGFS_CONTEXT_H
#define TAGFS_CONTEXT_H

#include "ktypes.h"

#define TAGFS_MAX_CONTEXT_TAGS 16

void tagfs_context_init(void);

int tagfs_context_add_tag(uint32_t pid, const char* tag_string);

int tagfs_context_remove_tag(uint32_t pid, const char* tag_string);

void tagfs_context_clear(uint32_t pid);

bool tagfs_context_matches(uint32_t pid, uint32_t file_id);

int tagfs_context_get_tags(uint32_t pid, const char* tags[], uint32_t max_tags);

void tagfs_context_destroy(uint32_t pid);

#endif // TAGFS_CONTEXT_H
