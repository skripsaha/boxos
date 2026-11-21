#ifndef TAG_REGISTRY_H
#define TAG_REGISTRY_H

#include "../tagfs.h"
#include "volume_tally.h"

/* Tag `sp` of `owner` as TAGS and a Coined carry it (volume_tally.h TallyTag):
 * the one way a tag of the registry is put into the book's form. The spans are
 * the spelling's: the tag holds while they do. */
static inline TallyTag TagRegistryBookForm(const TagSpelling* sp, TagOwner owner)
{
    bool owned = owner.kind != TAG_OWNER_PERSON;
    return (TallyTag){ .flags      = (sp->value ? TALLY_TAG_HAS_VALUE : 0u) | (owned ? TALLY_TAG_OWNED : 0u),
                       .owner_kind = owned ? owner.kind : 0u, .owner_id = owned ? owner.id : 0u,
                       .key = (const uint8_t*)sp->key, .key_len = sp->key_len,
                       .value = (const uint8_t*)sp->value, .value_len = sp->value_len };
}

/* Whose the book's tag `t` is: a program's when OWNED, else the person's. */
static inline TagOwner TagRegistryOwnerOf(const TallyTag* t)
{
    return (t->flags & TALLY_TAG_OWNED) ? TagOwnerProgram(t->owner_id) : TagOwnerPerson();
}

int          tag_registry_init(TagRegistry* reg);
void         tag_registry_destroy(TagRegistry* reg);
error_t      TagRegistryIntern(TagRegistry* reg, TagOwner owner, const TagSpelling* tag, uint32_t* out_id);
uint32_t     TagRegistryLookup(TagRegistry* reg, TagOwner owner, const TagSpelling* tag);
void         tag_registry_mark_system(TagRegistry* reg, uint32_t tag_id);
bool         TagRegistrySpelling(TagRegistry* reg, uint32_t tag_id, TagSpelling* out,
                                 bool* is_system, TagOwner* owner);
error_t      TagRegistryKeyIds(TagRegistry* reg, TagOwner owner, const char* key, size_t key_len,
                               TagIdList* out);
/* Whether `id` is one the registry gave — OK, so a record may wear it; or
 * ERR_TAGFS_METADATA_ERROR, or ERR_TAGFS_READ_ONLY under glass. */
error_t      TagRegistryIdGiven(TagRegistry* reg, uint32_t id);

/* The mount's: no id below `next` is given out again, and every id below the
 * next one given is the book's — none is laid down as a Coined. */
void         TagRegistryNumbersFrom(TagRegistry* reg, uint32_t next);

/* The stitch's (settle/sewing.c): the ids a stitch lays down as Coineds — every
 * id given since the last one a stitch that settled laid down, [*lo, *hi) —
 * and, its stitch settled, every id below `hi` the book's; and how many such
 * ids there are now, with the bytes they carry past a Coined's fixed fields —
 * keys, values and owners (the ribbon's room holds for them, settle/ribbon.c
 * RibbonClaim). */
void         TagRegistryCoinedRange(TagRegistry* reg, uint32_t* lo, uint32_t* hi);
void         TagRegistryCoinedUpTo(TagRegistry* reg, uint32_t hi);
void         TagRegistryCoinedHeld(TagRegistry* reg, uint32_t* count, uint64_t* text);

/* TagRoll — the registry's tags in the order of their ids: the first id in
 * [from, hi) the registry holds, its text in *out (resident, as the
 * registry keeps it) and whose it is in *owner; false when none does. A whole
 * Tally's TAGS is written by it (settle/binder_items.c). */
bool         TagRoll(TagRegistry* reg, uint32_t from, uint32_t hi, uint32_t* id, TagSpelling* out,
                     TagOwner* owner);

/* The tags the registry holds and the bytes they carry past their fixed
 * fields — keys, values, and a program's tag's owner — what a whole Tally's
 * TAGS is measured by (settle/endpaper.c). */
void         TagRegistryTextBytes(TagRegistry* reg, uint64_t* tags, uint64_t* bytes);

/* A tag the book's TAGS names, taken into the registry as the mount reads it:
 * OK; ERR_ALREADY_EXISTS when its owner and text are another id's or its id
 * another tag's; ERR_INVALID_ARGUMENT for an id past the registry's last;
 * ERR_NO_MEMORY. With the door shut. */
error_t      RegistryCoin(TagRegistry* reg, uint32_t tag_id, uint8_t flags, TagOwner owner,
                          const uint8_t* key_at, uint8_t key_len, const uint8_t* value_at, uint16_t value_len);

#endif // TAG_REGISTRY_H
