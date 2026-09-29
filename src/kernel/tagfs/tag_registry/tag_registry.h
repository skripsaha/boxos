#ifndef TAG_REGISTRY_H
#define TAG_REGISTRY_H

#include "../tagfs.h"

int          tag_registry_init(TagRegistry* reg);
void         tag_registry_destroy(TagRegistry* reg);
error_t      TagRegistryIntern(TagRegistry* reg, const TagSpelling* tag, uint16_t* out_id);
uint16_t     TagRegistryLookup(TagRegistry* reg, const TagSpelling* tag);
void         tag_registry_mark_system(TagRegistry* reg, uint16_t tag_id);
bool         TagRegistrySpelling(TagRegistry* reg, uint16_t tag_id, TagSpelling* out,
                                 bool* is_system);
error_t      TagRegistryKeyIds(TagRegistry* reg, const char* key, size_t key_len, TagIdList* out);
error_t      tag_registry_flush(TagRegistry* reg);
error_t      TagRegistryFlushFor(TagRegistry* reg, uint16_t id);
int          tag_registry_load(TagRegistry* reg, uint32_t first_block);

#endif // TAG_REGISTRY_H
