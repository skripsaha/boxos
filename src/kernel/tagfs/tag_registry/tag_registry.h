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
/* The flush need the registry's last written records hold: a Settle cycle's
 * first flush covers it, so a record never names a tag before the tag lies on
 * the medium. 0 when nothing was written this mount. */
uint64_t     tag_registry_written_need(TagRegistry* reg);

/* S0 of a Settle cycle, under the state lock: the registry's part of the
 * cycle's seal — the medium's last block of it and the bytes and CRC32 of the
 * records its last landed write left there. */
void         TagRegistryLanded(TagRegistry* reg, SettleMark* mark);

/* The mount's J1: no id below `next` is given out again — the records of the
 * pool wear ids a registry rolled back no longer names. */
void         TagRegistryNumbersFrom(TagRegistry* reg, uint32_t next);

/* The mount's census: the blocks the load walked, claimed, and let go. */
void         TagRegistryClaimGround(TagRegistry* reg);

#endif // TAG_REGISTRY_H
