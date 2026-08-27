#include "logbook.h"
#include "tagfs.h"      /* tagfs_parse_tag — pure string split, no volume */
#include "muster.h"

/*
 * One name the kernel has used for an occurrence. `value` is NULL for a bare
 * tag ("keyboard") and for the bare half of a valued one ("usb" out of
 * "usb:arrived"). Entries are never removed: a name the kernel has spoken
 * keeps its id for the life of the boot, which is what lets a driver cache a
 * handle at init and publish from an IRQ forever after.
 */
typedef struct LogbookEntry {
    struct LogbookEntry *chain;   /* hash bucket chain */
    char                *key;
    char                *value;   /* NULL = bare */
    TouchTag             tag_id;
    bool                 mustered; /* named before the voyage, not on first use */
    bool                 warned;   /* its absence from the muster has been said */
} LogbookEntry;

/*
 * Both tables grow; neither has a ceiling written into it. The hash doubles
 * and rehashes when it reaches one entry per bucket, the index doubles when
 * it fills. The only hard limit is the id space itself
 * (TOUCH_LOGBOOK_MAX_INDEX), and hitting it is a refusal, not a wrap.
 */
/* How many un-mustered names are worth a line before the point is made. */
#define LOGBOOK_MISS_LINES 8u
static uint32_t g_muster_misses = 0;

static struct {
    LogbookEntry **buckets;
    uint32_t       bucket_count;
    LogbookEntry **by_index;      /* index n -> entry; id = 0x8000 | n */
    uint32_t       index_capacity;
    uint32_t       count;
    spinlock_t     lock;          /* statically zeroed == unlocked */
    bool           ready;
} g_logbook;

static void logbook_muster_unlocked(void);
static void logbook_split(const char *tag, char *key, size_t key_size,
                          char *value, size_t value_size, bool *out_has_value);

#define LOGBOOK_INITIAL_BUCKETS  64u
#define LOGBOOK_INITIAL_INDEX    64u

/* Same djb2 the volume registry uses, so the two books hash names alike and
 * a reader comparing them by eye is not surprised. */
static uint32_t logbook_hash(const char *key, const char *value)
{
    uint32_t hash = 5381;
    while (*key) hash = ((hash << 5) + hash) + (uint8_t)(*key++);
    if (value) {
        hash = ((hash << 5) + hash) + ':';
        while (*value) hash = ((hash << 5) + hash) + (uint8_t)(*value++);
    }
    return hash;
}

static bool logbook_values_equal(const char *a, const char *b)
{
    if (!a && !b) return true;
    if (!a || !b) return false;
    return strcmp(a, b) == 0;
}

static char *logbook_copy(const char *src)
{
    if (!src) return NULL;
    size_t len = strlen(src) + 1;
    char *dst = kmalloc(len);
    if (!dst) return NULL;
    memcpy(dst, src, len);
    return dst;
}

/* Caller holds the lock. Builds the two tables on first use — there is no
 * init call and no boot phase to get wrong. */
static bool logbook_ready_unlocked(void)
{
    if (g_logbook.ready) return true;

    g_logbook.buckets = kmalloc(sizeof(LogbookEntry *) * LOGBOOK_INITIAL_BUCKETS);
    if (!g_logbook.buckets) return false;

    g_logbook.by_index = kmalloc(sizeof(LogbookEntry *) * LOGBOOK_INITIAL_INDEX);
    if (!g_logbook.by_index) {
        kfree(g_logbook.buckets);
        g_logbook.buckets = NULL;
        return false;
    }

    memset(g_logbook.buckets, 0, sizeof(LogbookEntry *) * LOGBOOK_INITIAL_BUCKETS);
    memset(g_logbook.by_index, 0, sizeof(LogbookEntry *) * LOGBOOK_INITIAL_INDEX);
    g_logbook.bucket_count   = LOGBOOK_INITIAL_BUCKETS;
    g_logbook.index_capacity = LOGBOOK_INITIAL_INDEX;
    g_logbook.count          = 0;
    g_logbook.ready          = true;

    logbook_muster_unlocked();
    return true;
}

static LogbookEntry *logbook_find_unlocked(const char *key, const char *value)
{
    if (!g_logbook.ready) return NULL;
    uint32_t slot = logbook_hash(key, value) % g_logbook.bucket_count;
    for (LogbookEntry *e = g_logbook.buckets[slot]; e; e = e->chain) {
        if (strcmp(e->key, key) == 0 && logbook_values_equal(e->value, value))
            return e;
    }
    return NULL;
}

/* Caller holds the lock. A failed grow is not fatal — the table keeps
 * working at its current width, chains just get longer. */
static void logbook_grow_buckets_unlocked(void)
{
    uint32_t new_count = g_logbook.bucket_count * 2;
    LogbookEntry **fresh = kmalloc(sizeof(LogbookEntry *) * new_count);
    if (!fresh) return;
    memset(fresh, 0, sizeof(LogbookEntry *) * new_count);

    for (uint32_t i = 0; i < g_logbook.bucket_count; i++) {
        LogbookEntry *e = g_logbook.buckets[i];
        while (e) {
            LogbookEntry *next = e->chain;
            uint32_t slot = logbook_hash(e->key, e->value) % new_count;
            e->chain = fresh[slot];
            fresh[slot] = e;
            e = next;
        }
    }

    kfree(g_logbook.buckets);
    g_logbook.buckets     = fresh;
    g_logbook.bucket_count = new_count;
}

/* Caller holds the lock. */
static bool logbook_reserve_index_unlocked(uint32_t index)
{
    while (index >= g_logbook.index_capacity) {
        uint32_t new_cap = g_logbook.index_capacity * 2;
        LogbookEntry **fresh = kmalloc(sizeof(LogbookEntry *) * new_cap);
        if (!fresh) return false;
        memcpy(fresh, g_logbook.by_index,
               sizeof(LogbookEntry *) * g_logbook.index_capacity);
        memset(fresh + g_logbook.index_capacity, 0,
               sizeof(LogbookEntry *) * (new_cap - g_logbook.index_capacity));
        kfree(g_logbook.by_index);
        g_logbook.by_index      = fresh;
        g_logbook.index_capacity = new_cap;
    }
    return true;
}

/* Caller holds the lock. Returns the id of an existing or freshly created
 * entry, TOUCH_TAG_INVALID if it could not be made. */
static TouchTag logbook_intern_unlocked(const char *key, const char *value,
                                        bool *out_created)
{
    if (out_created) *out_created = false;
    LogbookEntry *found = logbook_find_unlocked(key, value);
    if (found) return found->tag_id;

    if (g_logbook.count > TOUCH_LOGBOOK_MAX_INDEX) {
        kprintf("[Logbook] full at %u names — '%s' refused\n",
                g_logbook.count, key);
        return TOUCH_TAG_INVALID;
    }

    uint32_t index = g_logbook.count;
    if (!logbook_reserve_index_unlocked(index)) return TOUCH_TAG_INVALID;

    LogbookEntry *entry = kmalloc(sizeof(LogbookEntry));
    if (!entry) return TOUCH_TAG_INVALID;

    entry->key = logbook_copy(key);
    if (!entry->key) { kfree(entry); return TOUCH_TAG_INVALID; }

    entry->value = logbook_copy(value);
    if (value && !entry->value) {
        kfree(entry->key);
        kfree(entry);
        return TOUCH_TAG_INVALID;
    }

    entry->tag_id   = (TouchTag)(TOUCH_TAG_KERNEL_BIT | index);
    entry->mustered = false;
    entry->warned   = false;

    uint32_t slot = logbook_hash(key, value) % g_logbook.bucket_count;
    entry->chain = g_logbook.buckets[slot];
    g_logbook.buckets[slot] = entry;

    g_logbook.by_index[index] = entry;
    g_logbook.count = index + 1;

    if (g_logbook.count >= g_logbook.bucket_count)
        logbook_grow_buckets_unlocked();

    if (out_created) *out_created = true;
    return entry->tag_id;
}

/*
 * Call the muster — every name this kernel can speak, entered before anybody
 * asks for one. Caller holds the lock, and this runs exactly once, from the
 * same place the tables are built: there is no init call to forget and no boot
 * phase to get wrong, which is the property the rest of this file was written
 * for.
 *
 * A failure to seat a name is not fatal. It costs that ONE name the guarantee
 * the muster exists to give, and the line below says which.
 */
static void logbook_muster_unlocked(void)
{
    unsigned seated = 0, refused = 0;

    #define MUSTER_SEAT(NAME)                                                  \
        do {                                                                   \
            char k[256], v[256];                                               \
            bool has_value;                                                    \
            logbook_split((NAME), k, sizeof(k), v, sizeof(v), &has_value);     \
            if (k[0] != '\0') {                                               \
                LogbookEntry *e;                                               \
                if (logbook_intern_unlocked(k, NULL, NULL) == TOUCH_TAG_INVALID)\
                    refused++;                                                 \
                else { seated++;                                               \
                       e = logbook_find_unlocked(k, NULL);                     \
                       if (e) e->mustered = true; }                            \
                if (has_value) {                                               \
                    if (logbook_intern_unlocked(k, v, NULL) == TOUCH_TAG_INVALID)\
                        refused++;                                             \
                    else { seated++;                                           \
                           e = logbook_find_unlocked(k, v);                    \
                           if (e) e->mustered = true; }                        \
                }                                                              \
            }                                                                  \
        } while (0);

    TOUCH_KERNEL_MUSTER(MUSTER_SEAT)
    #undef MUSTER_SEAT

    kprintf("[Logbook] muster: %u name(s) seated%s\n", seated,
            refused ? ", SOME REFUSED — see above" : "");
}

/*
 * Split a tag string the same way the volume registry does, so a name means
 * the same thing in both books. "usb:arrived" -> key "usb", value "arrived";
 * "keyboard" -> key "keyboard", no value; "usb:..." -> the wildcard form,
 * which resolves to the bare id only.
 */
static void logbook_split(const char *tag, char *key, size_t key_size,
                          char *value, size_t value_size, bool *out_has_value)
{
    tagfs_parse_tag(tag, key, key_size, value, value_size);
    bool has_value = (value[0] != '\0');
    bool wildcard  = has_value && value[0] == '.' && value[1] == '.' &&
                                  value[2] == '.' && value[3] == '\0';
    *out_has_value = has_value && !wildcard;
}

static void logbook_resolve(const char *tag, TouchTag *out_full,
                            TouchTag *out_bare, bool create)
{
    *out_full = TOUCH_TAG_INVALID;
    *out_bare = TOUCH_TAG_INVALID;
    if (!tag || tag[0] == '\0') return;

    char key[256], value[256];
    bool has_value;
    logbook_split(tag, key, sizeof(key), value, sizeof(value), &has_value);
    if (key[0] == '\0') return;

    spin_lock(&g_logbook.lock);

    if (create && !logbook_ready_unlocked()) {
        spin_unlock(&g_logbook.lock);
        return;
    }

    if (create) {
        *out_bare = logbook_intern_unlocked(key, NULL, NULL);
        if (has_value) *out_full = logbook_intern_unlocked(key, value, NULL);

        /*
         * A name entered here and not at the muster is a name this kernel can
         * speak and never declared. It works — the entry exists from now on —
         * but the guarantee the muster gives is exactly the one it does not
         * have: anybody who asked for it EARLIER got an id out of the volume
         * instead, and is waiting on it.
         *
         * What counts as a miss is the BARE KEY, not the full name. A family
         * whose members are built from data — "pci:vendor:8086:1901", one per
         * device — is declared at the muster by its key alone, and every
         * member of it is then a name the muster covers. Judging the full name
         * instead made the muster's own entry worthless: the full name is new
         * for every device that has ever existed, so a correctly declared
         * family printed eight lines on every single boot, capped only because
         * the cap was there. It worked as written and was written wrong.
         *
         * So: said once, per key, and only for a key nobody declared. A family
         * missing from the muster is one line naming the family; a family in
         * it is silent, which is what having declared it is supposed to buy.
         */
        LogbookEntry *bare = logbook_find_unlocked(key, NULL);
        if (bare && !bare->mustered && !bare->warned &&
            g_muster_misses < LOGBOOK_MISS_LINES) {
            bare->warned = true;
            g_muster_misses++;
            kprintf("[Logbook] the kernel named '%s', which is not in its "
                    "muster — anybody who asked for it earlier is listening "
                    "elsewhere\n", tag);
        }
    } else {
        LogbookEntry *bare = logbook_find_unlocked(key, NULL);
        if (bare) *out_bare = bare->tag_id;
        if (has_value) {
            LogbookEntry *full = logbook_find_unlocked(key, value);
            if (full) *out_full = full->tag_id;
        }
    }

    spin_unlock(&g_logbook.lock);
}

void TouchLogbookResolve(const char *tag, TouchTag *out_full, TouchTag *out_bare)
{
    logbook_resolve(tag, out_full, out_bare, true);
}

void TouchLogbookLookup(const char *tag, TouchTag *out_full, TouchTag *out_bare)
{
    logbook_resolve(tag, out_full, out_bare, false);
}

TouchTag TouchLogbookIntern(const char *tag)
{
    TouchTag full, bare;
    TouchLogbookResolve(tag, &full, &bare);
    return (full != TOUCH_TAG_INVALID) ? full : bare;
}

const char *TouchLogbookName(TouchTag tag_id, const char **out_value)
{
    if (out_value) *out_value = NULL;
    if (!TouchTagIsKernel(tag_id)) return NULL;

    uint32_t index = (uint32_t)(tag_id & ~TOUCH_TAG_KERNEL_BIT);
    const char *key = NULL;

    spin_lock(&g_logbook.lock);
    if (g_logbook.ready && index < g_logbook.count && g_logbook.by_index[index]) {
        key = g_logbook.by_index[index]->key;
        if (out_value) *out_value = g_logbook.by_index[index]->value;
    }
    spin_unlock(&g_logbook.lock);

    return key;
}

uint32_t TouchLogbookCount(void)
{
    spin_lock(&g_logbook.lock);
    uint32_t n = g_logbook.count;
    spin_unlock(&g_logbook.lock);
    return n;
}
