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

/* One id of the registry as TagRoll reads it: the tag it names, or — once its
 * retirement began (TagRegistryRetire) — the id alone, `spelling` and `owner`
 * what the tag was, which no caller may use. */
typedef struct {
    uint32_t    id;
    TagSpelling spelling;
    TagOwner    owner;
    bool        retired;
    uint64_t    retired_gen;
} TagRolled;

/* Whether the book — as of the Ink `gen` — holds the id retired: the stitch or
 * the Tally that is of every Ink up to `gen` lays a tag whose files all let go
 * of it by then as RETIRED, and one whose retirement came later as it was. */
static inline bool TagRolledRetiredBy(const TagRolled* r, uint64_t gen)
{
    return r->retired && r->retired_gen <= gen;
}

/* The id as TAGS and a Coined carry it, as of the Ink `gen`. */
static inline TallyTag TagRolledBookForm(const TagRolled* r, uint64_t gen)
{
    if (TagRolledRetiredBy(r, gen))
        return (TallyTag){ .flags = TALLY_TAG_RETIRED };
    return TagRegistryBookForm(&r->spelling, r->owner);
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
 * and, its stitch settled, every id below `hi` the book's, and every
 * retirement it laid; and how many Coineds the registry owes now — those ids
 * and the retirements not yet laid — with the bytes the ids carry past a
 * Coined's fixed fields: keys, values and owners (the ribbon's room holds for
 * them, settle/ribbon.c RibbonClaim). */
void         TagRegistryCoinedRange(TagRegistry* reg, uint32_t* lo, uint32_t* hi);
void         TagRegistryCoinedUpTo(TagRegistry* reg, uint32_t hi, uint64_t gen);
void         TagRegistryCoinedHeld(TagRegistry* reg, uint32_t* count, uint64_t* text);

/* TagRoll — the registry's ids in the order of their ids: the first id in
 * [from, hi) the registry knows, as it is (TagRolled; the text resident, as
 * the registry keeps it); false when it knows none. A whole Tally's TAGS is
 * written by it (settle/binder_items.c). */
bool         TagRoll(TagRegistry* reg, uint32_t from, uint32_t hi, TagRolled* out);

/* The tags the registry knows and the bytes they carry past their fixed
 * fields — keys, values, and a program's tag's owner — that the book does not
 * yet hold retired: what a whole Tally's TAGS is measured by
 * (settle/endpaper.c). */
void         TagRegistryTextBytes(TagRegistry* reg, uint64_t* tags, uint64_t* bytes);

/*
 * ‼ A RETIRED TAG IS AN ID AND NOTHING MORE (settle/retire — a program that
 * left takes its own tags with it). Its retirement is in four steps, each
 * with its own caller: TagRegistryRetire closes the id — no lookup finds it,
 * no file may put it on — TagRegistryRetireRoom and TagRegistryRetireAt, once
 * no file wears it, lay it to be carried by a stitch (the Ink it is retired at
 * is what decides which stitch: TagRolledRetiredBy), and the stitch that
 * settles it lets it go from the list it owes (TagRegistryCoinedUpTo). The
 * mount takes an id the book holds retired as that at once: RegistryCoinRetired.
 * The text of a tag retired stays in memory until the registry is let go of,
 * since spans of it handed out before the retirement hold for as long as the
 * registry does (tagfs.h) — no question can reach it.
 */
error_t      TagRegistryRetire(TagRegistry* reg, uint32_t id);
error_t      TagRegistryRetireRoom(TagRegistry* reg, uint32_t count);
error_t      TagRegistryRetireAt(TagRegistry* reg, uint32_t id, uint64_t gen);
error_t      RegistryCoinRetired(TagRegistry* reg, uint32_t id);

/* Whether the id's retirement has begun, at any step. */
bool         TagRegistryRetiredId(TagRegistry* reg, uint32_t id);

/* The next retirement a stitch of every Ink up to `gen` lays below id `below`
 * — those of ids the book already has — from the list's place *at on: the id
 * in *id, true; false past the last. The list moves only when a stitch
 * settles, so the same stitch asking again is answered the same. */
bool         TagRegistryRetiringNext(TagRegistry* reg, uint32_t* at, uint32_t below, uint64_t gen, uint32_t* id);

/* A program's tag: its id and the fid of the program. */
typedef struct {
    uint32_t id;
    uint64_t fid;
} TagProgramTag;

/* Every program's tag whose retirement is not finished, COPIED: *out is the
 * caller's to kfree, NULL with 0 for none. ERR_NO_MEMORY when the heap would
 * not give the room. */
error_t      TagRegistryProgramTags(TagRegistry* reg, TagProgramTag** out, uint32_t* count);

/* A tag the book's TAGS names, taken into the registry as the mount reads it:
 * OK; ERR_ALREADY_EXISTS when its owner and text are another id's or its id
 * another tag's; ERR_INVALID_ARGUMENT for an id past the registry's last;
 * ERR_NO_MEMORY. With the door shut. */
error_t      RegistryCoin(TagRegistry* reg, uint32_t tag_id, uint8_t flags, TagOwner owner,
                          const uint8_t* key_at, uint8_t key_len, const uint8_t* value_at, uint16_t value_len);

#endif // TAG_REGISTRY_H
