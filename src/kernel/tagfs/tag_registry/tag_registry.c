#include "tag_registry.h"
#include "../tagfs_constants.h"

/*
 * The volume's tags in memory: every tag the book names, and every one given
 * since. A tag is never renamed or taken back, and its id is what every file
 * on the volume carries, so an id is given once and stays its tag's. The book
 * holds them — its Tally's TAGS, and the Coineds of the stitches settled since
 * (settle/sewing.c) — and nothing here touches the medium. Everything is the
 * instance's own: a registry built beside the volume's — a test's, the
 * manifest's self-test — shares nothing with it but the code.
 *
 * ‼ NOTHING UNDER THE REGISTRY'S LOCK TOUCHES THE MEDIUM, TAKES A BLOCK OR
 * WAITS.
 *
 * The lock keeps interrupts off, and every core that names a tag takes it —
 * Touch, a cabin, a process, a create. A registry that grows its hashes builds
 * the new ones outside the lock and swaps them in under it.
 */

struct TagKeyGroup {
    char*               key;
    uint32_t*           tag_ids;
    uint32_t            count;
    uint32_t            capacity;
    struct TagKeyGroup* next;
};

static uint32_t key_hash(const char* key, size_t key_len, uint32_t bucket_count) {
    TagSpelling bare = { key, NULL, key_len, 0 };
    return TagSpellingHash(&bare) % bucket_count;
}

/* The entry of `id` in an id's hash of `buckets` buckets; NULL when no tag
 * has it. */
static TagRegistryEntry* RegistryById(TagRegistryEntry* const* by_id, uint32_t buckets,
                                      uint32_t id) {
    TagRegistryEntry* entry = by_id[id % buckets];
    while (entry && entry->tag_id != id)
        entry = entry->id_next;
    return entry;
}

/* Whether an entry is the tag spelled: the same key, and the same value or
 * none on both sides. */
static bool entry_is(const TagRegistryEntry* entry, const TagSpelling* tag) {
    if (entry->key_len != tag->key_len || memcmp(entry->key, tag->key, tag->key_len) != 0)
        return false;
    if (!entry->value || !tag->value)
        return !entry->value && !tag->value;
    return entry->value_len == tag->value_len &&
           memcmp(entry->value, tag->value, tag->value_len) == 0;
}

/* False when the group had to grow and there was no memory for it. */
static bool add_to_key_group(TagKeyGroup* group, uint32_t tag_id) {
    if (group->count >= group->capacity) {
        uint32_t new_cap = group->capacity * 2;
        uint32_t* new_ids = kmalloc(sizeof(uint32_t) * new_cap);
        if (!new_ids) {
            debug_printf("[TagRegistry] add_to_key_group: grow alloc failed\n");
            return false;
        }
        memcpy(new_ids, group->tag_ids, sizeof(uint32_t) * group->count);
        kfree(group->tag_ids);
        group->tag_ids = new_ids;
        group->capacity = new_cap;
    }
    group->tag_ids[group->count++] = tag_id;
    return true;
}

static TagKeyGroup* find_or_create_key_group(TagRegistry* reg, const char* key, size_t key_len) {
    uint32_t bucket = key_hash(key, key_len, reg->key_bucket_count);
    TagKeyGroup* group = reg->key_buckets[bucket];

    while (group) {
        if (TagSpanIs(group->key, key, key_len)) return group;
        group = group->next;
    }

    TagKeyGroup* new_group = kmalloc(sizeof(TagKeyGroup));
    if (!new_group) {
        debug_printf("[TagRegistry] find_or_create_key_group: group alloc failed\n");
        return NULL;
    }

    new_group->key = TagSpanDup(key, key_len);
    if (!new_group->key) {
        kfree(new_group);
        debug_printf("[TagRegistry] find_or_create_key_group: key copy failed\n");
        return NULL;
    }

    new_group->tag_ids = kmalloc(sizeof(uint32_t) * 8);
    if (!new_group->tag_ids) {
        kfree(new_group->key);
        kfree(new_group);
        debug_printf("[TagRegistry] find_or_create_key_group: tag_ids alloc failed\n");
        return NULL;
    }

    new_group->count    = 0;
    new_group->capacity = 8;
    new_group->next     = reg->key_buckets[bucket];
    reg->key_buckets[bucket] = new_group;
    reg->key_groups++;

    return new_group;
}

/*
 * ‼ THE HASHES GROW WITH THE TAGS.
 *
 * They were a fixed TAGFS_REG_BUCKETS and TAGFS_KEY_BUCKETS, so a registry of
 * thirty thousand tags walked chains sixty long on every lookup — under a lock
 * every core that names a tag takes with its interrupts off. Each doubles now
 * once it holds more than it has buckets: the new table is taken with no lock
 * held, filled and put in place under the lock, and the old one given back
 * after it. A table the heap will not give keeps the chains longer until the
 * next tag asks again, and nothing else is lost. The text's hash and the id's
 * grow together — both or neither — since they share their size.
 */
/* Whether either hash holds more than it has buckets. Under reg->lock. */
static bool RegistryCrowded(const TagRegistry* reg) {
    return reg->total_tags > reg->bucket_count || reg->key_groups > reg->key_bucket_count;
}

static void RegistryRehash(TagRegistry* reg) {
    TagFSLockTake(&reg->lock);
    uint32_t buckets     = reg->bucket_count;
    uint32_t key_buckets = reg->key_bucket_count;
    uint32_t want        = buckets;
    uint32_t key_want    = key_buckets;
    while (reg->total_tags > want) want *= 2;
    while (reg->key_groups > key_want) key_want *= 2;
    TagFSLockDrop(&reg->lock);
    if (want == buckets && key_want == key_buckets) return;

    TagRegistryNode**  table     = want != buckets ? kmalloc(sizeof(TagRegistryNode*) * want) : NULL;
    TagRegistryEntry** id_table  = table ? kmalloc(sizeof(TagRegistryEntry*) * want) : NULL;
    TagKeyGroup**      key_table = key_want != key_buckets ? kmalloc(sizeof(TagKeyGroup*) * key_want) : NULL;
    if (!id_table) {
        kfree(table);
        table = NULL;
    }
    if (table) memset(table, 0, sizeof(TagRegistryNode*) * want);
    if (id_table) memset(id_table, 0, sizeof(TagRegistryEntry*) * want);
    if (key_table) memset(key_table, 0, sizeof(TagKeyGroup*) * key_want);

    TagFSLockTake(&reg->lock);
    if (table && reg->bucket_count == buckets) {
        for (uint32_t b = 0; b < buckets; b++) {
            for (TagRegistryEntry* entry = reg->by_id[b], *next; entry; entry = next) {
                uint32_t at    = entry->tag_id % want;
                next           = entry->id_next;
                entry->id_next = id_table[at];
                id_table[at]   = entry;
            }
        }
        for (uint32_t b = 0; b < buckets; b++) {
            for (TagRegistryNode* node = reg->buckets[b], *next; node; node = next) {
                const TagRegistryEntry* entry = RegistryById(id_table, want, node->tag_id);
                TagSpelling spelled = { entry->key, entry->value, entry->key_len, entry->value_len };
                uint32_t    at      = TagSpellingHash(&spelled) % want;
                next       = node->next;
                node->next = table[at];
                table[at]  = node;
            }
        }
        TagRegistryNode**  old    = reg->buckets;
        TagRegistryEntry** old_id = reg->by_id;
        reg->buckets      = table;
        reg->by_id        = id_table;
        reg->bucket_count = want;
        table             = old;
        id_table          = old_id;
    }
    if (key_table && reg->key_bucket_count == key_buckets) {
        for (uint32_t b = 0; b < key_buckets; b++) {
            for (TagKeyGroup* group = reg->key_buckets[b], *next; group; group = next) {
                uint32_t at = key_hash(group->key, strlen(group->key), key_want);
                next          = group->next;
                group->next   = key_table[at];
                key_table[at] = group;
            }
        }
        TagKeyGroup** old = reg->key_buckets;
        reg->key_buckets      = key_table;
        reg->key_bucket_count = key_want;
        key_table             = old;
    }
    TagFSLockDrop(&reg->lock);
    kfree(table);
    kfree(id_table);
    kfree(key_table);
}

static uint32_t lookup_unlocked(TagRegistry* reg, const TagSpelling* tag) {
    uint32_t bucket = TagSpellingHash(tag) % reg->bucket_count;
    TagRegistryNode* node = reg->buckets[bucket];

    while (node) {
        if (entry_is(RegistryById(reg->by_id, reg->bucket_count, node->tag_id), tag)) {
            return node->tag_id;
        }
        node = node->next;
    }

    return TAGFS_INVALID_TAG_ID;
}

/*
 * The entry for a tag under `tag_id`, linked into every index: the text's
 * hash, the id's and the key's group. Everything is allocated before anything
 * is linked, so ERR_NO_MEMORY leaves the registry as it was. Under reg->lock.
 */
static error_t RegistryEnter(TagRegistry* reg, const TagSpelling* tag, uint32_t tag_id,
                             uint8_t flags) {
    TagRegistryEntry* entry = kmalloc(sizeof(TagRegistryEntry));
    TagRegistryNode*  node  = kmalloc(sizeof(TagRegistryNode));
    char*             key   = TagSpanDup(tag->key, tag->key_len);
    char*             value = tag->value ? TagSpanDup(tag->value, tag->value_len) : NULL;
    TagKeyGroup*      group = NULL;

    if (entry && node && key && (!tag->value || value))
        group = find_or_create_key_group(reg, tag->key, tag->key_len);
    if (!group || !add_to_key_group(group, tag_id)) {
        kfree(value);
        kfree(key);
        kfree(node);
        kfree(entry);
        return ERR_NO_MEMORY;
    }

    entry->key       = key;
    entry->value     = value;
    entry->key_len   = (uint8_t)tag->key_len;
    entry->value_len = (uint16_t)tag->value_len;
    entry->flags     = flags;
    entry->tag_id    = tag_id;

    uint32_t bucket = TagSpellingHash(tag) % reg->bucket_count;
    node->tag_id = tag_id;
    node->next   = reg->buckets[bucket];
    reg->buckets[bucket] = node;
    entry->id_next = reg->by_id[tag_id % reg->bucket_count];
    reg->by_id[tag_id % reg->bucket_count] = entry;

    if (tag_id >= reg->next_id) reg->next_id = tag_id + 1;
    reg->total_tags++;
    reg->text_bytes += (uint64_t)entry->key_len + entry->value_len;
    return OK;
}

/*
 * ‼ A BOOK'S TAG IS TAKEN WITH A VERDICT, NEVER DROPPED. A book's TAGS name
 * each id once and each text once; one whose text is another id's, or whose
 * id another text holds, is a book this kernel cannot hold as it is, and the
 * caller is told so (ERR_ALREADY_EXISTS), so the volume stands under glass
 * with the reason rather than with one of the two names quietly lost. The
 * width is the caller's to have judged (tag_spelling.h).
 */
error_t RegistryCoin(TagRegistry* reg, uint32_t tag_id, uint8_t flags, const uint8_t* key_at,
                     uint8_t key_len, const uint8_t* value_at, uint16_t value_len)
{
    TagSpelling tag = { (const char*)key_at, (flags & TAGFS_TAG_FLAG_HAS_VALUE) ? (const char*)value_at : NULL,
                        key_len, value_len };
    if (tag_id > TAGFS_MAX_TAG_ID)
        return ERR_INVALID_ARGUMENT;
    if (lookup_unlocked(reg, &tag) != TAGFS_INVALID_TAG_ID ||
        RegistryById(reg->by_id, reg->bucket_count, tag_id))
        return ERR_ALREADY_EXISTS;
    return RegistryEnter(reg, &tag, tag_id, flags);
}

/* The tag's id, a new one under next_id when the registry has none. Under
 * reg->lock; the caller has judged the widths. */
static error_t intern_unlocked(TagRegistry* reg, const TagSpelling* tag, uint32_t* out_id) {
    uint32_t existing = lookup_unlocked(reg, tag);
    if (existing != TAGFS_INVALID_TAG_ID) {
        *out_id = existing;
        return OK;
    }
    if (reg->next_id > TAGFS_MAX_TAG_ID)
        return ERR_TAGFS_REGISTRY_FULL;

    uint32_t id = reg->next_id;
    error_t  rc = RegistryEnter(reg, tag, id, tag->value ? TAGFS_TAG_FLAG_HAS_VALUE : 0);
    if (rc == OK) {
        *out_id = id;
        if (id >= reg->coined_next)
            reg->coined_text += (uint64_t)tag->key_len + tag->value_len;
    }
    return rc;
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

int tag_registry_init(TagRegistry* reg) {
    reg->buckets = kmalloc(sizeof(TagRegistryNode*) * TAGFS_REG_BUCKETS);
    if (!reg->buckets) {
        debug_printf("[TagRegistry] init: buckets alloc failed\n");
        return -1;
    }
    memset(reg->buckets, 0, sizeof(TagRegistryNode*) * TAGFS_REG_BUCKETS);

    reg->by_id = kmalloc(sizeof(TagRegistryEntry*) * TAGFS_REG_BUCKETS);
    if (!reg->by_id) {
        kfree(reg->buckets);
        debug_printf("[TagRegistry] init: by_id alloc failed\n");
        return -1;
    }
    memset(reg->by_id, 0, sizeof(TagRegistryEntry*) * TAGFS_REG_BUCKETS);

    reg->key_buckets = kmalloc(sizeof(TagKeyGroup*) * TAGFS_KEY_BUCKETS);
    if (!reg->key_buckets) {
        kfree(reg->by_id);
        kfree(reg->buckets);
        debug_printf("[TagRegistry] init: key_buckets alloc failed\n");
        return -1;
    }
    memset(reg->key_buckets, 0, sizeof(TagKeyGroup*) * TAGFS_KEY_BUCKETS);

    reg->bucket_count     = TAGFS_REG_BUCKETS;
    reg->key_bucket_count = TAGFS_KEY_BUCKETS;
    reg->key_groups       = 0;
    reg->total_tags       = 0;
    reg->text_bytes       = 0;
    reg->next_id          = 0;
    reg->coined_next      = 0;
    reg->coined_text      = 0;
    reg->said_full        = false;

    TagFSLockInit(&reg->lock, "registry", LOCK_RANK_LEAF);

    debug_printf("[TagRegistry] initialized: %u reg buckets, %u key buckets\n",
                 TAGFS_REG_BUCKETS, TAGFS_KEY_BUCKETS);
    return 0;
}

void tag_registry_destroy(TagRegistry* reg) {
    if (!reg) return;

    for (uint32_t i = 0; i < reg->bucket_count; i++) {
        TagRegistryNode* node = reg->buckets[i];
        while (node) {
            TagRegistryNode* next = node->next;
            kfree(node);
            node = next;
        }
    }
    /* Every entry is in exactly one bucket of the id's hash, which has as many
     * buckets as the text's. */
    for (uint32_t i = 0; i < reg->bucket_count; i++) {
        TagRegistryEntry* entry = reg->by_id[i];
        while (entry) {
            TagRegistryEntry* next = entry->id_next;
            kfree(entry->key);
            if (entry->value) kfree(entry->value);
            kfree(entry);
            entry = next;
        }
    }
    kfree(reg->buckets);
    reg->buckets = NULL;
    kfree(reg->by_id);
    reg->by_id = NULL;

    for (uint32_t i = 0; i < reg->key_bucket_count; i++) {
        TagKeyGroup* group = reg->key_buckets[i];
        while (group) {
            TagKeyGroup* next = group->next;
            kfree(group->key);
            kfree(group->tag_ids);
            kfree(group);
            group = next;
        }
    }
    kfree(reg->key_buckets);
    reg->key_buckets = NULL;

    debug_printf("[TagRegistry] destroyed\n");
}

error_t TagRegistryIntern(TagRegistry* reg, const TagSpelling* tag, uint32_t* out_id) {
    if (!out_id) return ERR_NULL_POINTER;
    *out_id = TAGFS_INVALID_TAG_ID;
    if (!reg || !tag || !tag->key) return ERR_NULL_POINTER;

    /* A tag wider than the book carries one is refused before it has a
     * number or any memory (tag_spelling.h). */
    error_t rc = TagSpellingFits(tag->key_len, tag->value_len);
    if (rc != OK)
        return rc;

    TagFSLockTake(&reg->lock);
    rc = intern_unlocked(reg, tag, out_id);
    bool crowded = RegistryCrowded(reg);
    TagFSLockDrop(&reg->lock);
    if (crowded)
        RegistryRehash(reg);

    /* Said once for the registry: the ids of a volume are never given back,
     * so a registry that is full stays full, and every later refusal is the
     * same fact. */
    if (rc == ERR_TAGFS_REGISTRY_FULL && !__atomic_exchange_n(&reg->said_full, true, __ATOMIC_ACQ_REL))
        kprintf("[TagRegistry] this volume has given out all %u of the tag numbers its book can "
                "carry, and a new tag cannot have one — every tag not yet known to it is refused "
                "from now on (error %d)\n", TAGFS_MAX_TAG_ID + 1u, (int)rc);
    return rc;
}

uint32_t TagRegistryLookup(TagRegistry* reg, const TagSpelling* tag) {
    if (!reg || !tag || !tag->key) return TAGFS_INVALID_TAG_ID;
    TagFSLockTake(&reg->lock);
    uint32_t id = lookup_unlocked(reg, tag);
    TagFSLockDrop(&reg->lock);
    return id;
}

bool TagRegistrySpelling(TagRegistry* reg, uint32_t tag_id, TagSpelling* out, bool* is_system) {
    if (!reg || !out) return false;
    TagFSLockTake(&reg->lock);
    const TagRegistryEntry* entry = RegistryById(reg->by_id, reg->bucket_count, tag_id);
    if (entry) {
        out->key       = entry->key;
        out->key_len   = entry->key_len;
        out->value     = entry->value;
        out->value_len = entry->value_len;
        if (is_system)
            *is_system = (entry->flags & TAGFS_TAG_FLAG_SYSTEM) != 0;
    }
    TagFSLockDrop(&reg->lock);
    return entry != NULL;
}

/* The ids are given one after another and never taken back, so the walk is
 * the ids themselves: a span of them a hold of the leaf lock. A span that
 * finds none is a hole in the numbers — a Coined of another writer may name an
 * id far past the rest — and the least id past it is looked for among the
 * tags held instead, never walked to id by id. */
#define TAG_ROLL_SPAN  256u

/* The least id in [from, hi) a tag has, UINT32_MAX for none: every bucket of
 * the id's hash looked through, a span of them a hold of the lock — and from
 * the first again when the table grew meanwhile. */
static uint32_t TagRollLeast(TagRegistry* reg, uint32_t from, uint32_t hi)
{
    uint32_t best = UINT32_MAX, buckets = 0, b = 0;
    for (bool done = false; !done;) {
        TagFSLockTake(&reg->lock);
        if (buckets != reg->bucket_count) {
            buckets = reg->bucket_count;
            b       = 0;
            best    = UINT32_MAX;
        }
        uint32_t end = buckets - b > TAG_ROLL_SPAN ? b + TAG_ROLL_SPAN : buckets;
        for (; b < end; b++)
            for (const TagRegistryEntry* e = reg->by_id[b]; e; e = e->id_next)
                if (e->tag_id >= from && e->tag_id < hi && e->tag_id < best)
                    best = e->tag_id;
        done = b == buckets;
        TagFSLockDrop(&reg->lock);
    }
    return best;
}

/* The tag of `t`, when one has it, into *id and *out. */
static bool TagRollAt(TagRegistry* reg, uint32_t t, uint32_t* id, TagSpelling* out)
{
    const TagRegistryEntry* entry = RegistryById(reg->by_id, reg->bucket_count, t);
    if (!entry)
        return false;
    *id            = t;
    out->key       = entry->key;
    out->key_len   = entry->key_len;
    out->value     = entry->value;
    out->value_len = entry->value_len;
    return true;
}

bool TagRoll(TagRegistry* reg, uint32_t from, uint32_t hi, uint32_t* id, TagSpelling* out) {
    if (!reg || !out || from >= hi) return false;
    uint32_t end   = hi - from > TAG_ROLL_SPAN ? from + TAG_ROLL_SPAN : hi;
    bool     found = false;
    TagFSLockTake(&reg->lock);
    for (uint32_t t = from; t < end && !found; t++)
        found = TagRollAt(reg, t, id, out);
    TagFSLockDrop(&reg->lock);
    if (found || end == hi) return found;
    uint32_t least = TagRollLeast(reg, end, hi);
    if (least == UINT32_MAX) return false;
    TagFSLockTake(&reg->lock);
    found = TagRollAt(reg, least, id, out);
    TagFSLockDrop(&reg->lock);
    return found;
}

error_t TagRegistryKeyIds(TagRegistry* reg, const char* key, size_t key_len, TagIdList* out) {
    if (!out) return ERR_NULL_POINTER;
    out->ids      = NULL;
    out->count    = 0;
    out->capacity = 0;
    if (!reg || !key) return ERR_NULL_POINTER;

    /* Sized and copied under the lock the next tag of the key takes to free
     * the group's array: the copy is whole and the array is never seen after
     * the lock goes. A group holds at most TAGFS_MAX_TAG_ID + 1 ids, which a
     * TagIdList counts. */
    error_t rc = OK;
    TagFSLockTake(&reg->lock);
    TagKeyGroup* group = reg->key_buckets[key_hash(key, key_len, reg->key_bucket_count)];
    while (group && !TagSpanIs(group->key, key, key_len))
        group = group->next;
    if (group && group->count > 0) {
        out->ids = kmalloc(sizeof(uint32_t) * group->count);
        if (out->ids) {
            memcpy(out->ids, group->tag_ids, sizeof(uint32_t) * group->count);
            out->count    = group->count;
            out->capacity = group->count;
        } else {
            rc = ERR_NO_MEMORY;
        }
    }
    TagFSLockDrop(&reg->lock);
    return rc;
}

// Stamp an entry as a reserved-vocabulary (system) tag. Idempotent OR of the
// SYSTEM bit; it never makes the registry owe the medium anything — the bit
// is re-derived from TagFsReservedKeys at every mount, so it must never be
// the reason for a write.
void tag_registry_mark_system(TagRegistry* reg, uint32_t tag_id) {
    if (!reg || tag_id == TAGFS_INVALID_TAG_ID) return;
    TagFSLockTake(&reg->lock);
    TagRegistryEntry* entry = RegistryById(reg->by_id, reg->bucket_count, tag_id);
    if (entry)
        entry->flags |= TAGFS_TAG_FLAG_SYSTEM;
    TagFSLockDrop(&reg->lock);
}

// ----------------------------------------------------------------------------
// The book's numbers
// ----------------------------------------------------------------------------

/*
 * Whether `id` is one the registry gave: a tag lies in the stitch of the cycle
 * that settles the record wearing it, as a Coined ahead of the record (spec_v2
 * §2.8) — the cycle takes every id given before the record's Ink
 * (TagRegistryCoinedRange) — so nothing is written here. A volume under glass
 * takes no write, and a tag it does not hold yet is one no record of it may
 * wear. Called with no TagFS lock held.
 */
error_t TagRegistryIdGiven(TagRegistry* reg, uint32_t id) {
    if (!reg) return ERR_NULL_POINTER;
    if (TagFSIsUnderGlass()) return ERR_TAGFS_READ_ONLY;
    TagFSLockTake(&reg->lock);
    bool given = id < reg->next_id;
    TagFSLockDrop(&reg->lock);
    return given ? OK : ERR_TAGFS_METADATA_ERROR;
}

/*
 * ‼ THE NUMBERS MOVE, AND NOTHING IS SIZED BY THEM.
 *
 * next_id moves past every id the book names (book_mount.c). It used to bound
 * a table indexed by id, which had
 * to grow to cover the numbers before they moved, or every reader of an id
 * past its end read the heap's next block (Z08-1). The id's hash holds only
 * the entries there are, so an id no tag has is found absent at any height,
 * and moving the numbers takes nothing. Never past what the book can carry
 * (TAGFS_MAX_TAG_ID).
 */
void TagRegistryNumbersFrom(TagRegistry* reg, uint32_t next) {
    TagFSLockTake(&reg->lock);
    if (next > TAGFS_MAX_TAG_ID + 1u)
        next = TAGFS_MAX_TAG_ID + 1u;
    if (next > reg->next_id)
        reg->next_id = next;
    reg->coined_next = reg->next_id;
    reg->coined_text = 0;
    TagFSLockDrop(&reg->lock);
}

void TagRegistryCoinedRange(TagRegistry* reg, uint32_t* lo, uint32_t* hi) {
    TagFSLockTake(&reg->lock);
    *lo = reg->coined_next;
    *hi = reg->next_id;
    TagFSLockDrop(&reg->lock);
}

void TagRegistryCoinedUpTo(TagRegistry* reg, uint32_t hi) {
    TagFSLockTake(&reg->lock);
    for (uint32_t id = reg->coined_next; id < hi && id < reg->next_id; id++) {
        const TagRegistryEntry* e = RegistryById(reg->by_id, reg->bucket_count, id);
        uint64_t                text = e ? (uint64_t)e->key_len + e->value_len : 0;
        reg->coined_text = reg->coined_text > text ? reg->coined_text - text : 0;
    }
    if (hi > reg->coined_next)
        reg->coined_next = hi;
    TagFSLockDrop(&reg->lock);
}

void TagRegistryCoinedHeld(TagRegistry* reg, uint32_t* count, uint64_t* text) {
    TagFSLockTake(&reg->lock);
    *count = reg->next_id > reg->coined_next ? reg->next_id - reg->coined_next : 0;
    *text  = reg->coined_text;
    TagFSLockDrop(&reg->lock);
}

void TagRegistryTextBytes(TagRegistry* reg, uint64_t* tags, uint64_t* bytes) {
    TagFSLockTake(&reg->lock);
    *tags  = reg->total_tags;
    *bytes = reg->text_bytes;
    TagFSLockDrop(&reg->lock);
}
