#include "tag_registry.h"

/*
 * ‼ THE MEDIUM HOLDS THE RECORD OF EVERY ID BELOW written_next, AND ONLY EVER
 * GAINS MORE.
 *
 * A tag is never renamed or taken back, and its id is what every file on the
 * volume carries, so the registry on the medium only ever grows at its end:
 * the records of the ids below written_next, in id order, in the chain that
 * ends at tail_block. A flush appends what was interned since — to the tail,
 * and to one new block at a time when the tail is full — and never writes a
 * block the medium already holds whole again. A registry that owes nothing
 * writes nothing. Everything about the medium is the instance's own: a
 * registry built beside the volume's — a test's, the manifest's self-test —
 * shares nothing with it but the code.
 *
 * ‼ NOTHING UNDER THE REGISTRY'S LOCK TOUCHES THE MEDIUM, TAKES A BLOCK OR
 * WAITS.
 *
 * The lock keeps interrupts off, and every core that names a tag takes it —
 * Touch, a cabin, a process, a create. The flush used to hold it across every
 * block it wrote, which kept all of them spinning with interrupts off for as
 * long as the medium took, and across the block a growing chain takes, which
 * put the volume's state lock under the registry's while a rename takes the
 * registry's under state: two cores meeting there both ways round never moved
 * again (MP-D4). So a flush copies what it owes into its images under the
 * lock, lets the lock go, and writes; and one flush of a registry runs at a
 * time under the registry's Scribe, which a second flush waits for instead of
 * spinning.
 *
 * ‼ A NEW BLOCK IS NAMED ONLY AFTER THE MEDIUM HAS KEPT ITS STAMP (scribe.h,
 * THE SPARE RULE).
 *
 * The chain used to grow link first: the tail went down naming a block the
 * medium had not been given yet, and a refused or lost write of that block
 * left a chain into unstamped ground, which every later mount refuses
 * (MP-D5). Now the new block is stamped, a kept flush proves the stamp, the
 * block takes its records, and only then does the tail go down with the link
 * — so a cut anywhere leaves the chain as it was, or longer by a stamped
 * block. The block a growth took stays the registry's until a link names it:
 * a flush that failed keeps it for the next one (CR-11), because the tail it
 * could not write may have landed after all.
 */

struct TagKeyGroup {
    char*               key;
    uint16_t*           tag_ids;
    uint32_t            count;
    uint32_t            capacity;
    struct TagKeyGroup* next;
};

static uint32_t key_hash(const char* key, size_t key_len, uint32_t bucket_count) {
    TagSpelling bare = { key, NULL, key_len, 0 };
    return TagSpellingHash(&bare) % bucket_count;
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
static bool add_to_key_group(TagKeyGroup* group, uint16_t tag_id) {
    if (group->count >= group->capacity) {
        uint32_t new_cap = group->capacity * 2;
        uint16_t* new_ids = kmalloc(sizeof(uint16_t) * new_cap);
        if (!new_ids) {
            debug_printf("[TagRegistry] add_to_key_group: grow alloc failed\n");
            return false;
        }
        memcpy(new_ids, group->tag_ids, sizeof(uint16_t) * group->count);
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

    new_group->tag_ids = kmalloc(sizeof(uint16_t) * 8);
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
 * ‼ THE TWO HASHES GROW WITH THE TAGS.
 *
 * They were a fixed TAGFS_REG_BUCKETS and TAGFS_KEY_BUCKETS, so a registry of
 * thirty thousand tags walked chains sixty long on every lookup — under a lock
 * every core that names a tag takes with its interrupts off. Each doubles now
 * once it holds more than it has buckets: the new table is taken with no lock
 * held, filled and put in place under the lock, and the old one given back
 * after it. A table the heap will not give keeps the chains longer until the
 * next tag asks again, and nothing else is lost.
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

    TagRegistryNode** table     = want != buckets ? kmalloc(sizeof(TagRegistryNode*) * want) : NULL;
    TagKeyGroup**     key_table = key_want != key_buckets ? kmalloc(sizeof(TagKeyGroup*) * key_want) : NULL;
    if (table) memset(table, 0, sizeof(TagRegistryNode*) * want);
    if (key_table) memset(key_table, 0, sizeof(TagKeyGroup*) * key_want);

    TagFSLockTake(&reg->lock);
    if (table && reg->bucket_count == buckets) {
        for (uint32_t b = 0; b < buckets; b++) {
            for (TagRegistryNode* node = reg->buckets[b], *next; node; node = next) {
                const TagRegistryEntry* entry = reg->by_id[node->tag_id];
                TagSpelling spelled = { entry->key, entry->value, entry->key_len, entry->value_len };
                uint32_t    at      = TagSpellingHash(&spelled) % want;
                next       = node->next;
                node->next = table[at];
                table[at]  = node;
            }
        }
        TagRegistryNode** old = reg->buckets;
        reg->buckets      = table;
        reg->bucket_count = want;
        table             = old;
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
    kfree(key_table);
}

static uint16_t lookup_unlocked(TagRegistry* reg, const TagSpelling* tag) {
    uint32_t bucket = TagSpellingHash(tag) % reg->bucket_count;
    TagRegistryNode* node = reg->buckets[bucket];

    while (node) {
        if (entry_is(reg->by_id[node->tag_id], tag)) {
            return node->tag_id;
        }
        node = node->next;
    }

    return TAGFS_INVALID_TAG_ID;
}

// Grow by_id array to accommodate at least `needed_id`
static int ensure_by_id_capacity(TagRegistry* reg, uint16_t needed_id) {
    while (needed_id >= reg->by_id_capacity) {
        uint32_t new_cap = reg->by_id_capacity * 2;
        TagRegistryEntry** new_by_id = kmalloc(sizeof(TagRegistryEntry*) * new_cap);
        if (!new_by_id) {
            debug_printf("[TagRegistry] ensure_by_id_capacity: alloc failed\n");
            return -1;
        }
        memcpy(new_by_id, reg->by_id, sizeof(TagRegistryEntry*) * reg->by_id_capacity);
        memset(new_by_id + reg->by_id_capacity, 0,
               sizeof(TagRegistryEntry*) * (new_cap - reg->by_id_capacity));
        kfree(reg->by_id);
        reg->by_id = new_by_id;
        reg->by_id_capacity = new_cap;
    }
    return 0;
}

/*
 * The entry for a tag under `tag_id`, linked into every index: the hash, the
 * id table (grown already) and the key's group. Everything is allocated before
 * anything is linked, so ERR_NO_MEMORY leaves the registry as it was. Under
 * reg->lock.
 */
static error_t RegistryEnter(TagRegistry* reg, const TagSpelling* tag, uint16_t tag_id,
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
    reg->by_id[tag_id]   = entry;

    if (tag_id >= reg->next_id) reg->next_id = tag_id + 1;
    reg->total_tags++;
    return OK;
}

/*
 * One record read off the medium, taken in under the id the medium gives it —
 * the id every file wearing the tag carries, so it is never renumbered. The
 * rules are the loader's, in its order: an exact (key, value) the registry
 * already holds keeps its id and takes the medium's flags; an id past
 * TAGFS_MAX_TAG_ID, or one another tag already holds, is dropped. A record's
 * key and value lie in its block as they will lie in memory, so they are
 * compared there and copied only when kept. Under reg->lock. ERR_NO_MEMORY
 * only when memory ran out.
 */
static error_t RegistryAdopt(TagRegistry* reg, uint16_t tag_id, uint8_t flags,
                             const uint8_t* key_at, uint8_t key_len,
                             const uint8_t* value_at, uint16_t value_len) {
    TagSpelling tag = { (const char*)key_at, value_len ? (const char*)value_at : NULL,
                        key_len, value_len };

    uint16_t held = lookup_unlocked(reg, &tag);
    if (held != TAGFS_INVALID_TAG_ID) {
        reg->by_id[held]->flags = flags;
        return OK;
    }
    if (tag_id > TAGFS_MAX_TAG_ID)
        return OK;
    if (ensure_by_id_capacity(reg, tag_id) != 0)
        return ERR_NO_MEMORY;
    if (reg->by_id[tag_id]) {
        debug_printf("[TagRegistry] load: slot %u already used\n", tag_id);
        return OK;
    }
    return RegistryEnter(reg, &tag, tag_id, flags);
}

/* The tag's id, a new one under next_id when the registry has none. Under
 * reg->lock; the caller has judged the widths. */
static error_t intern_unlocked(TagRegistry* reg, const TagSpelling* tag, uint16_t* out_id) {
    uint16_t existing = lookup_unlocked(reg, tag);
    if (existing != TAGFS_INVALID_TAG_ID) {
        *out_id = existing;
        return OK;
    }
    if (reg->next_id > TAGFS_MAX_TAG_ID)
        return ERR_TAGFS_REGISTRY_FULL;
    if (ensure_by_id_capacity(reg, reg->next_id) != 0)
        return ERR_NO_MEMORY;

    uint16_t id = reg->next_id;
    error_t  rc = RegistryEnter(reg, tag, id, tag->value ? TAGFS_TAG_FLAG_HAS_VALUE : 0);
    if (rc == OK)
        *out_id = id;
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

    reg->by_id = kmalloc(sizeof(TagRegistryEntry*) * 64);
    if (!reg->by_id) {
        kfree(reg->buckets);
        debug_printf("[TagRegistry] init: by_id alloc failed\n");
        return -1;
    }
    memset(reg->by_id, 0, sizeof(TagRegistryEntry*) * 64);

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
    reg->by_id_capacity   = 64;
    reg->total_tags       = 0;
    reg->next_id          = 0;

    TagFSLockInit(&reg->lock, "registry", LOCK_RANK_LEAF);

    /* No medium yet: tag_registry_load reads one in, and until it has there is
     * nothing to write into and nothing owed to it. */
    reg->written_next = 0;
    reg->tail_block   = 0;
    reg->tail_image   = NULL;
    reg->spare_block  = 0;
    reg->spare_image  = NULL;
    reg->spare_need   = 0;
    reg->said_failing = false;
    reg->said_full    = false;
    reg->scribe       = (Scribe)SCRIBE_INIT(SCRIBE_RANK_REGISTRY, "the tag registry");

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
    kfree(reg->buckets);
    reg->buckets = NULL;

    for (uint32_t i = 0; i < reg->next_id; i++) {
        TagRegistryEntry* entry = reg->by_id[i];
        if (entry) {
            kfree(entry->key);
            if (entry->value) kfree(entry->value);
            kfree(entry);
        }
    }
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

    kfree(reg->tail_image);
    reg->tail_image = NULL;
    kfree(reg->spare_image);
    reg->spare_image = NULL;

    debug_printf("[TagRegistry] destroyed\n");
}

error_t TagRegistryIntern(TagRegistry* reg, const TagSpelling* tag, uint16_t* out_id) {
    if (!out_id) return ERR_NULL_POINTER;
    *out_id = TAGFS_INVALID_TAG_ID;
    if (!reg || !tag || !tag->key) return ERR_NULL_POINTER;

    /* A record never crosses the edge of a block, so a tag whose record would
     * not fit an empty one could never be written — and a flush that owed it
     * would never be done. Refused before it has a number or any memory. */
    error_t rc = TagSpellingFits(tag->key_len, tag->value_len);
    if (rc != OK)
        return rc;

    TagFSLockTake(&reg->lock);
    rc = intern_unlocked(reg, tag, out_id);
    bool crowded = RegistryCrowded(reg);
    TagFSLockDrop(&reg->lock);
    if (crowded)
        RegistryRehash(reg);

    /* Said once for the registry: the ids of a v1 volume are never given back,
     * so a registry that is full stays full, and every later refusal is the
     * same fact. */
    if (rc == ERR_TAGFS_REGISTRY_FULL && !__atomic_exchange_n(&reg->said_full, true, __ATOMIC_ACQ_REL))
        kprintf("[TagRegistry] this volume has given out all %u of its tag numbers, and a new "
                "tag cannot have one — every tag not yet known to it is refused from now on "
                "(error %d)\n", TAGFS_MAX_TAG_ID + 1u, (int)rc);
    return rc;
}

uint16_t TagRegistryLookup(TagRegistry* reg, const TagSpelling* tag) {
    if (!reg || !tag || !tag->key) return TAGFS_INVALID_TAG_ID;
    TagFSLockTake(&reg->lock);
    uint16_t id = lookup_unlocked(reg, tag);
    TagFSLockDrop(&reg->lock);
    return id;
}

bool TagRegistrySpelling(TagRegistry* reg, uint16_t tag_id, TagSpelling* out, bool* is_system) {
    if (!reg || !out) return false;
    TagFSLockTake(&reg->lock);
    const TagRegistryEntry* entry = tag_id < reg->next_id ? reg->by_id[tag_id] : NULL;
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
        out->ids = kmalloc(sizeof(uint16_t) * group->count);
        if (out->ids) {
            memcpy(out->ids, group->tag_ids, sizeof(uint16_t) * group->count);
            out->count    = (uint16_t)group->count;
            out->capacity = (uint16_t)group->count;
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
void tag_registry_mark_system(TagRegistry* reg, uint16_t tag_id) {
    if (!reg || tag_id == TAGFS_INVALID_TAG_ID) return;
    TagFSLockTake(&reg->lock);
    if (tag_id < reg->next_id && reg->by_id[tag_id])
        reg->by_id[tag_id]->flags |= TAGFS_TAG_FLAG_SYSTEM;
    TagFSLockDrop(&reg->lock);
}

// ----------------------------------------------------------------------------
// The flush
// ----------------------------------------------------------------------------

/*
 * The records of the ids from `from` on, appended to `image` while each one
 * fits whole — the order and the packing mkfs and every kernel before this one
 * wrote, so an append lands exactly where a rewrite would have. An id no tag
 * holds (one the medium did not have) is passed over. Answers the first id it
 * did not take: next_id when every one fitted. Under reg->lock.
 */
static uint16_t RegistryPack(TagRegistry* reg, TagRegistryBlock* image, uint16_t from) {
    uint16_t id = from;

    for (; id < reg->next_id; id++) {
        const TagRegistryEntry* entry = reg->by_id[id];
        if (!entry)
            continue;

        uint8_t  key_len   = entry->key_len;
        uint16_t value_len = entry->value_len;
        uint32_t size      = TAGFS_REGISTRY_RECORD_HEAD + key_len + value_len;
        if (image->used_bytes + size > TAGFS_REGISTRY_DATA_SIZE)
            break;

        uint8_t* p = image->data + image->used_bytes;
        memcpy(p, &id, sizeof(id));
        p[2] = entry->flags;
        p[3] = key_len;
        memcpy(p + 4, &value_len, sizeof(value_len));
        memcpy(p + TAGFS_REGISTRY_RECORD_HEAD, entry->key, key_len);
        if (value_len)
            memcpy(p + TAGFS_REGISTRY_RECORD_HEAD + key_len, entry->value, value_len);

        image->used_bytes = (uint16_t)(image->used_bytes + size);
        image->entry_count++;
    }
    return id;
}

/*
 * The chain lengthened by one block, for the ids from `from` on that the tail
 * had no room for: the Spare stamped when none is in hand, a kept flush
 * proving the stamp, then the records packed into the Spare and written. OK:
 * the Spare is whole on the medium, *out_end is the first id it did not take,
 * and the tail's image links it — a link that reaches the medium only with the
 * tail, after all of this. Otherwise nothing names the Spare, and one the
 * medium has stamped stays in hand for the next flush; *why says what refused.
 */
static error_t RegistryGrow(TagRegistry* reg, uint16_t from, uint16_t* out_end,
                            char* why, size_t why_size) {
    error_t rc = OK;

    if (reg->spare_block == 0) {
        rc = SpareStamp(TAGFS_REGISTRY_MAGIC, reg->spare_image, &reg->spare_block, &reg->spare_need);
        if (rc == ERR_TAGFS_NO_SPACE) {
            ksnprintf(why, why_size, "the volume has no free block to lengthen the chain with");
            return rc;
        }
        if (rc != OK) {
            ksnprintf(why, why_size, "the block taken to lengthen the chain would not take its "
                                     "stamp, and was given back");
            return rc;
        }
    }
    rc = VolumeFlushFor(reg->spare_need);
    if (rc != OK) {
        ksnprintf(why, why_size, "the medium would not flush the stamp of block %u, the chain's next",
                  reg->spare_block);
        return rc;
    }

    memset(reg->spare_image, 0, TAGFS_BLOCK_SIZE);
    reg->spare_image->magic = TAGFS_REGISTRY_MAGIC;
    TagFSLockTake(&reg->lock);
    uint16_t end = RegistryPack(reg, reg->spare_image, from);
    TagFSLockDrop(&reg->lock);

    rc = tagfs_write_block(reg->spare_block, reg->spare_image);
    if (rc != OK) {
        ksnprintf(why, why_size, "block %u, the chain's next, would not take them", reg->spare_block);
        return rc;
    }
    reg->tail_image->next_block = reg->spare_block;
    *out_end = end;
    return OK;
}

/*
 * One pass: what is owed appended to the tail, and — when the tail is full —
 * to one new block the tail then links, the tail's write last, because it is
 * the one that commits. The holder of reg->scribe, no lock held.
 *
 * OK: every id below the new written_next is on the medium. ERR_TAGFS_NO_SPACE:
 * the volume has no block to grow by, and the tail took what fitted — a full
 * volume keeps the tags it has room for. Any other refusal moves nothing: the
 * tail's image is put back, written_next stays, and the next pass packs the
 * same records at the same places and links the same Spare.
 */
static error_t RegistryWritePass(TagRegistry* reg, char* why, size_t why_size) {
    TagRegistryBlock* tail      = reg->tail_image;
    uint16_t          was_used  = tail->used_bytes;
    uint16_t          was_count = tail->entry_count;

    TagFSLockTake(&reg->lock);
    uint16_t upto = RegistryPack(reg, tail, reg->written_next);
    bool     full = upto < reg->next_id;
    TagFSLockDrop(&reg->lock);

    uint16_t end = upto;
    error_t  rc  = full ? RegistryGrow(reg, upto, &end, why, why_size) : OK;
    if (rc != OK && rc != ERR_TAGFS_NO_SPACE)
        goto refused;

    if (tail->entry_count != was_count || tail->next_block != 0) {
        error_t wrote = tagfs_write_block(reg->tail_block, tail);
        if (wrote != OK) {
            ksnprintf(why, why_size, "block %u, the chain's last, would not take them", reg->tail_block);
            rc = wrote;
            goto refused;
        }
    }

    TagFSLockTake(&reg->lock);
    reg->written_next = tail->next_block ? end : upto;
    TagFSLockDrop(&reg->lock);
    if (tail->next_block) {
        reg->tail_block  = reg->spare_block;
        reg->tail_image  = reg->spare_image;
        reg->spare_image = tail;
        reg->spare_block = 0;
    }
    return rc;

refused:
    /* A refused write of the tail may have landed all the same, so the Spare
     * is never given back while the volume is mounted: a link to it may be on
     * the medium, and a block handed to a file there is a chain the next
     * mount refuses. Keeping it is what makes a retry link the same block. */
    tail->used_bytes  = was_used;
    tail->entry_count = was_count;
    tail->next_block  = 0;
    memset(tail->data + was_used, 0, TAGFS_REGISTRY_DATA_SIZE - was_used);
    return rc;
}

error_t tag_registry_flush(TagRegistry* reg) {
    if (!reg) return ERR_NULL_POINTER;

    TagFSLockTake(&reg->lock);
    bool owed = reg->written_next < reg->next_id;
    TagFSLockDrop(&reg->lock);
    if (!owed)
        return OK;
    if (!reg->tail_image)
        return ERR_NOT_INITIALIZED;     /* no medium was ever read into it */

    /* One flush of a registry at a time, and a flush asked for while another
     * runs is answered by the pass that covers it — its own, or one that
     * began after it asked. What a pass writes is what is owed when it takes
     * the Scribe, so every tag interned before the ask is in it. */
    uint64_t cover  = 0;
    error_t  rc     = OK;
    uint64_t ticket = ScribeAsk(&reg->scribe);
    switch (ScribeWait(&reg->scribe, ticket, &cover, &rc)) {
    case SCRIBE_MINE: {
        char why[112];

        TagFSLockTake(&reg->lock);
        uint16_t target = reg->next_id;
        TagFSLockDrop(&reg->lock);
        /* written_next moves only in the hands of this Scribe's holder. */
        while (rc == OK && reg->written_next < target)
            rc = RegistryWritePass(reg, why, sizeof(why));

        if (rc == OK) {
            reg->said_failing = false;
        } else if (!reg->said_failing) {
            reg->said_failing = true;
            TagFSLockTake(&reg->lock);
            uint32_t unwritten = (uint32_t)(reg->next_id - reg->written_next);
            TagFSLockDrop(&reg->lock);
            kprintf("[TagRegistry] %u tag id(s) this volume issued are not on its medium yet — %s "
                    "(error %d) — they stay in memory, and the next flush writes them\n",
                    unwritten, why, (int)rc);
        }
        ScribeGive(&reg->scribe, cover, rc);
        break;
    }
    case SCRIBE_SELF:
        kprintf("[TagRegistry] flush refused: a flush of this registry on this core is suspended "
                "beneath this one, and waiting for it here would never end\n");
        rc = ERR_BUSY;
        break;
    default:
        break;      /* WRITTEN leaves OK; REFUSED has put the pass's refusal in rc */
    }
    return rc;
}

/*
 * One id's record on the medium: OK at once when the medium already holds it
 * (below written_next), otherwise a flush, and OK only if that flush put it
 * there. The flush's own refusal otherwise — so a caller about to make a file
 * wear the id can refuse instead, and no record on the medium ever wears an id
 * the registry on the medium does not define. Called with no TagFS lock held.
 */
error_t TagRegistryFlushFor(TagRegistry* reg, uint16_t id) {
    if (!reg) return ERR_NULL_POINTER;

    TagFSLockTake(&reg->lock);
    bool held = id < reg->written_next;
    TagFSLockDrop(&reg->lock);
    if (held)
        return OK;

    error_t rc = tag_registry_flush(reg);

    TagFSLockTake(&reg->lock);
    held = id < reg->written_next;
    TagFSLockDrop(&reg->lock);
    if (held)
        return OK;
    return rc != OK ? rc : ERR_TAGFS_METADATA_ERROR;
}

// ----------------------------------------------------------------------------
// The load
// ----------------------------------------------------------------------------

int tag_registry_load(TagRegistry* reg, uint32_t first_block) {
    if (!reg) return -1;

    /*
     * ‼ THE ONLY BOUND ON THIS WALK IS A TERMINATION PROOF, NOT A LENGTH.
     *
     * The same rule the metadata pool and the file table are already under. A
     * chain of distinct blocks cannot be longer than the data run it lives in,
     * so a walk that takes that many hops has come back to a block it has
     * already been to: the hop count proves termination AND catches the cycle,
     * and no visited-set can refuse one hop sooner.
     *
     * It had neither bound. A next_block pointing outside the volume was
     * followed off the end of it — nothing under tagfs_read_block bounds a
     * block number — and a next_block pointing back into the chain span for
     * ever, which is a machine that never finishes mounting rather than one
     * that says what is wrong. Both are reachable on a volume whose registry
     * block took a bad sector, and both became reachable in practice the day
     * mkfs started chaining the registry.
     *
     * The registry is NOT read out of layout.tag_registry_blocks for the
     * reason stated over that field in create_tagfs: a number written once
     * when the volume was made is not a length that ordinary use keeps true.
     */
    TagFSState* fs  = tagfs_get_state();
    uint32_t    run = fs ? fs->layout.data_blocks : 0;
    if (run == 0) {
        kprintf("[TagRegistry] this volume states a data run of no blocks at "
                "all, so there is nowhere for a tag registry to be — it is not "
                "mounted\n");
        return -1;
    }

    /* The two blocks every flush of this registry is written through, and the
     * one every block of the chain is read into on the way: the heap's, not
     * the stack's. The walk ends with the tail in tail_image, which is then
     * the medium's own copy. */
    reg->tail_image  = kmalloc(TAGFS_BLOCK_SIZE);
    reg->spare_image = kmalloc(TAGFS_BLOCK_SIZE);
    if (!reg->tail_image || !reg->spare_image) {
        kprintf("[TagRegistry] there is no memory for the two blocks this registry is read and "
                "written through — this volume is not mounted\n");
        return -1;
    }

    /* Read with no lock held: nobody can see this registry yet — the mount
     * reads it before its door opens, and a registry a test builds is the
     * test's. The lock is taken only for what goes into memory. */
    TagRegistryBlock* image  = reg->tail_image;
    uint32_t          block  = first_block;
    uint32_t          hops   = 0;
    uint16_t          walked = 0;
    uint32_t          end    = 0;
    for (;;) {
        if (block >= run) {
            kprintf("[TagRegistry] the chain points at block %u and this "
                    "volume's data run is %u blocks — that is outside the "
                    "volume, so it is not mounted\n", block, run);
            return -1;
        }
        error_t read = tagfs_read_block(block, image);
        if (read != OK) {
            kprintf("[TagRegistry] block %u of the tag registry would not read (error %d) — this "
                    "volume is not mounted\n", block, (int)read);
            return -1;
        }
        if (image->magic != TAGFS_REGISTRY_MAGIC) {
            kprintf("[TagRegistry] block %u holds 0x%08x where the tag registry should be — this "
                    "volume is not mounted\n", block, image->magic);
            return -1;
        }

        /* Walked the way every reader walks it: entry_count records packed from
         * the start of the data, none past its end. */
        error_t held = OK;
        walked = 0;
        end    = 0;
        TagFSLockTake(&reg->lock);
        while (held == OK && walked < image->entry_count &&
               end + TAGFS_REGISTRY_RECORD_HEAD <= TAGFS_REGISTRY_DATA_SIZE) {
            const uint8_t* rec = image->data + end;
            uint16_t       tag_id, value_len;
            memcpy(&tag_id, rec, sizeof(tag_id));
            memcpy(&value_len, rec + 4, sizeof(value_len));
            uint32_t size = TAGFS_REGISTRY_RECORD_HEAD + rec[3] + value_len;
            if (end + size > TAGFS_REGISTRY_DATA_SIZE)
                break;
            held = RegistryAdopt(reg, tag_id, rec[2], rec + TAGFS_REGISTRY_RECORD_HEAD, rec[3],
                                 rec + TAGFS_REGISTRY_RECORD_HEAD + rec[3], value_len);
            end += size;
            walked++;
        }
        TagFSLockDrop(&reg->lock);
        if (held != OK) {
            kprintf("[TagRegistry] there is no memory to hold the tags of block %u — a registry "
                    "held in part would give their numbers to new tags, so this volume is not "
                    "mounted\n", block);
            return -1;
        }
        RegistryRehash(reg);

        if (image->next_block == 0) break;  /* end of chain */

        if (++hops >= run) {
            kprintf("[TagRegistry] this volume's tag registry chain has taken "
                    "%u hops in a data run of %u blocks, so it is going round "
                    "in a circle — it is not mounted\n", hops, run);
            return -1;
        }
        block = image->next_block;
    }

    /* The tail as the next flush appends to it: its records end where the walk
     * ended and nothing follows them. Whatever lay past a record the walk
     * stopped at was never read by any reader, and the first append writes
     * over it. Nothing is owed: the medium holds every id this registry has. */
    image->entry_count = walked;
    image->used_bytes  = (uint16_t)end;
    memset(image->data + end, 0, TAGFS_REGISTRY_DATA_SIZE - end);

    TagFSLockTake(&reg->lock);
    reg->tail_block   = block;
    reg->written_next = reg->next_id;
    TagFSLockDrop(&reg->lock);
    return 0;
}
