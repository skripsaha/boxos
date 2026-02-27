#ifndef TAGFS_CONTEXT_H
#define TAGFS_CONTEXT_H

#include "ktypes.h"

#define TAGFS_MAX_CONTEXT_TAGS 16

// Initialize context system
void tagfs_context_init(void);

// Add tag to process context
int tagfs_context_add_tag(uint32_t pid, const char* tag_string);

// Remove tag from process context
int tagfs_context_remove_tag(uint32_t pid, const char* tag_string);

// Clear all tags for process
void tagfs_context_clear(uint32_t pid);

// Check if file matches context for process
bool tagfs_context_matches(uint32_t pid, uint32_t file_id);

// Get active tags for process
int tagfs_context_get_tags(uint32_t pid, const char* tags[], uint32_t max_tags);

// Destroy context for process (cleanup on exit)
void tagfs_context_destroy(uint32_t pid);

#endif // TAGFS_CONTEXT_H
