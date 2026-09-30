#ifndef TAG_REGISTRY_INTERNAL_H
#define TAG_REGISTRY_INTERNAL_H

#include "tag_registry.h"

/*
 * The registry's own parts, shared by tag_registry.c (the book in memory and
 * its flush) and tag_registry_load.c (the load, the seal's part, the census).
 */

/* A record read off the medium entered at its own id — dropped when its
 * spelling is already held, its id is past TAGFS_MAX_TAG_ID or taken. Under
 * reg->lock; ERR_NO_MEMORY only when memory ran out. */
error_t RegistryAdopt(TagRegistry* reg, uint16_t tag_id, uint8_t flags,
                      const uint8_t* key_at, uint8_t key_len,
                      const uint8_t* value_at, uint16_t value_len);

/* Both hashes grown to what the registry holds; takes reg->lock itself. */
void    RegistryRehash(TagRegistry* reg);

/* The blocks the load walked, let go. */
void    RegistryChainLetGo(TagRegistry* reg);

#endif /* TAG_REGISTRY_INTERNAL_H */
