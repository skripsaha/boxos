#ifndef TAG_REGISTRY_H
#define TAG_REGISTRY_H

#include "../tagfs.h"

int          tag_registry_init(TagRegistry* reg);
void         tag_registry_destroy(TagRegistry* reg);
error_t      TagRegistryIntern(TagRegistry* reg, const TagSpelling* tag, uint32_t* out_id);
uint32_t     TagRegistryLookup(TagRegistry* reg, const TagSpelling* tag);
void         tag_registry_mark_system(TagRegistry* reg, uint32_t tag_id);
bool         TagRegistrySpelling(TagRegistry* reg, uint32_t tag_id, TagSpelling* out,
                                 bool* is_system);
error_t      TagRegistryKeyIds(TagRegistry* reg, const char* key, size_t key_len, TagIdList* out);
error_t      tag_registry_flush(TagRegistry* reg);
error_t      TagRegistryFlushFor(TagRegistry* reg, uint32_t id);
/* The flush need the registry's last written records hold: a Settle cycle's
 * first flush covers it, so a record never names a tag before the tag lies on
 * the medium. 0 when nothing was written this mount. */
uint64_t     tag_registry_written_need(TagRegistry* reg);

/* S0 of a Settle cycle, under the state lock: the registry's part of the
 * cycle's seal — the medium's last block of it and the bytes and CRC32 of the
 * records its last landed write left there. */
void         TagRegistryLanded(TagRegistry* reg, SettleMark* mark);

/* The mount's J1: no id below `next` is given out again — the records of the
 * pool wear ids a registry rolled back no longer names. On format v2 every id
 * below the next one given is the book's: none is laid down as a Coined. */
void         TagRegistryNumbersFrom(TagRegistry* reg, uint32_t next);

/* Format v2 (settle/sewing.c): the ids a stitch lays down as Coineds — every
 * id given since the last one a stitch that settled laid down, [*lo, *hi) —
 * and, its stitch settled, every id below `hi` the book's; and how many such
 * ids there are now, with the bytes of their keys and values (the ribbon's
 * room holds for them, settle/ribbon.c RibbonClaim). */
void         TagRegistryCoinedRange(TagRegistry* reg, uint32_t* lo, uint32_t* hi);
void         TagRegistryCoinedUpTo(TagRegistry* reg, uint32_t hi);
void         TagRegistryCoinedHeld(TagRegistry* reg, uint32_t* count, uint64_t* text);

/* TagRoll — the registry's tags in the order of their ids: the first id in
 * [from, hi) the registry holds, its text in *out (resident, as the
 * registry keeps it); false when none does. A whole Tally's TAGS is written
 * by it (settle/binder_items.c). */
bool         TagRoll(TagRegistry* reg, uint32_t from, uint32_t hi, uint32_t* id, TagSpelling* out);

/* The tags the registry holds and the bytes of their keys and values — what
 * a whole Tally's TAGS is measured by (settle/endpaper.c). */
void         TagRegistryTextBytes(TagRegistry* reg, uint64_t* tags, uint64_t* bytes);

/* A tag a format-v2 book's TAGS names, taken into the registry as the mount
 * reads it: OK; ERR_ALREADY_EXISTS when its text is another id's or its id
 * another text's; ERR_INVALID_ARGUMENT for an id past the registry's last;
 * ERR_NO_MEMORY. With the door shut, as the v1 load. */
error_t      RegistryCoin(TagRegistry* reg, uint32_t tag_id, uint8_t flags, const uint8_t* key_at,
                          uint8_t key_len, const uint8_t* value_at, uint16_t value_len);

/* The mount's census: the blocks the load walked, claimed, and let go. */
void         TagRegistryClaimGround(TagRegistry* reg);

#endif // TAG_REGISTRY_H
