#ifndef TAG_REGISTRY_H
#define TAG_REGISTRY_H

#include "../tagfs.h"

int          tag_registry_init(TagRegistry* reg);
void         tag_registry_destroy(TagRegistry* reg);
uint16_t     tag_registry_intern(TagRegistry* reg, const char* key, const char* value);
uint16_t     tag_registry_lookup(TagRegistry* reg, const char* key, const char* value);
const char*  tag_registry_key(TagRegistry* reg, uint16_t tag_id);
const char*  tag_registry_value(TagRegistry* reg, uint16_t tag_id);
TagKeyGroup* tag_registry_key_group(TagRegistry* reg, const char* key);
int          tag_registry_flush(TagRegistry* reg);
int          tag_registry_load(TagRegistry* reg, uint32_t first_block);

#endif // TAG_REGISTRY_H
