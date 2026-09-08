#include "use_context.h"
#include "tagfs.h"
#include "klib.h"
#include "atomics.h"
#include "process.h"
#include "cabin.h"

/*
 * One tag of the context: its name, and the volume's number for it if the
 * volume has one. TAGFS_INVALID_TAG_ID means unbound — the volume that is up
 * (or the absence of one) has no page for this name yet.
 */
typedef struct UseTag
{
    char    *text;
    uint16_t id;
} UseTag;

/*
 * The whole context. Everything below `lock` is written under it; the three
 * fields the scheduler reads without it — ready, bits, overflow_count — are
 * stored atomically, and a reader that needs the overflow list itself takes
 * the lock. `overflow` is allocated with room for every tag of the context at
 * the moment the context is set, so nothing here allocates under the lock.
 */
static struct
{
    UseTag   *tags;
    uint32_t  count;
    uint32_t  unbound;      /* tags with no volume number */
    uint16_t *overflow;     /* numbers >= 64, room for `count` of them */
    uint16_t  overflow_count;
    uint64_t  bits;         /* numbers < 64 */
    volatile uint32_t ready; /* 1 = count > 0 && unbound == 0 */
    spinlock_t lock;
} g_use;

/* The volume registry's field widths (TagRegistryEntry on disk: a one-byte
 * key length; every TagFS parse buffer is 256). A tag that cannot be written
 * to the volume cannot be a context tag either. */
#define USE_TAG_PART_MAX 255u

/* -------------------------------------------------------------------------
 * Names
 * ------------------------------------------------------------------------- */

/* What tag_normalise made of a raw tag. */
typedef enum
{
    TAG_BLANK   = 0,    /* nothing but blanks — the caller skips it */
    TAG_SPELLED = 1,    /* a tag, written canonically into `out` */
    TAG_UNFIT   = 2,    /* a key or value the registry could not hold */
} TagSpelling;

/*
 * One raw tag out of a list — as typed, blanks and all — into its canonical
 * spelling: "key" or "key:value".
 */
static TagSpelling tag_normalise(const char *raw, size_t len, char *out, size_t out_cap)
{
    while (len > 0 && (raw[0] == ' ' || raw[0] == '\t')) { raw++; len--; }
    while (len > 0 && (raw[len - 1] == ' ' || raw[len - 1] == '\t')) len--;
    if (len == 0) return TAG_BLANK;

    const char *colon = NULL;
    for (size_t i = 0; i < len; i++) {
        if (raw[i] == ':') { colon = raw + i; break; }
    }

    size_t klen = colon ? (size_t)(colon - raw) : len;
    size_t vlen = colon ? len - klen - 1 : 0;
    if (klen == 0 || klen > USE_TAG_PART_MAX || vlen > USE_TAG_PART_MAX)
        return TAG_UNFIT;

    /* "key:" says the same thing as "key"; the registry has one entry for it. */
    size_t need = klen + (vlen ? vlen + 1 : 0) + 1;
    if (need > out_cap) return TAG_UNFIT;

    memcpy(out, raw, klen);
    if (vlen) {
        out[klen] = ':';
        memcpy(out + klen + 1, colon + 1, vlen);
    }
    out[need - 1] = '\0';
    return TAG_SPELLED;
}

static void tags_free(UseTag *tags, uint32_t count)
{
    if (!tags) return;
    for (uint32_t i = 0; i < count; i++) kfree(tags[i].text);
    kfree(tags);
}

/*
 * Parse a comma-separated list into an array of unbound tags. A duplicate is
 * kept once. On any failure nothing is kept and the error is the caller's to
 * report.
 */
static error_t list_parse(const char *list, UseTag **out_tags, uint32_t *out_count)
{
    *out_tags  = NULL;
    *out_count = 0;
    if (!list) return OK;

    uint32_t capacity = 0, count = 0;
    UseTag  *tags = NULL;
    char     spelled[USE_TAG_PART_MAX * 2 + 2];

    const char *pos = list;
    for (;;) {
        const char *comma = strchr(pos, ',');
        size_t len = comma ? (size_t)(comma - pos) : strlen(pos);

        TagSpelling spelling = tag_normalise(pos, len, spelled, sizeof(spelled));
        if (spelling == TAG_UNFIT) { tags_free(tags, count); return ERR_INVALID_ARGUMENT; }
        if (spelling == TAG_SPELLED) {
            bool seen = false;
            for (uint32_t i = 0; i < count && !seen; i++)
                seen = strcmp(tags[i].text, spelled) == 0;

            if (!seen) {
                if (count == capacity) {
                    uint32_t grown_cap = capacity ? capacity * 2 : 4;
                    UseTag  *grown = kmalloc(sizeof(UseTag) * grown_cap);
                    if (!grown) { tags_free(tags, count); return ERR_NO_MEMORY; }
                    if (count) memcpy(grown, tags, sizeof(UseTag) * count);
                    kfree(tags);
                    tags     = grown;
                    capacity = grown_cap;
                }
                size_t bytes = strlen(spelled) + 1;
                char  *text  = kmalloc(bytes);
                if (!text) { tags_free(tags, count); return ERR_NO_MEMORY; }
                memcpy(text, spelled, bytes);
                tags[count].text = text;
                tags[count].id   = TAGFS_INVALID_TAG_ID;
                count++;
            }
        }

        if (!comma) break;
        pos = comma + 1;
    }

    *out_tags  = tags;
    *out_count = count;
    return OK;
}

/* -------------------------------------------------------------------------
 * The scheduler's cache
 * ------------------------------------------------------------------------- */

/* Recount bits / overflow / unbound from the tags' numbers. Under the lock;
 * `overflow` already has room for every tag. */
static void cache_rebuild_locked(void)
{
    uint64_t bits     = 0;
    uint16_t overflow = 0;
    uint32_t unbound  = 0;

    for (uint32_t i = 0; i < g_use.count; i++) {
        uint16_t id = g_use.tags[i].id;
        if (id == TAGFS_INVALID_TAG_ID)   unbound++;
        else if (id < 64)                 bits |= (uint64_t)1 << id;
        else                              g_use.overflow[overflow++] = id;
    }

    /* ready goes down before the numbers move and up after they have moved,
     * so a reader that sees ready sees numbers of one context. */
    __atomic_store_n(&g_use.ready, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_use.bits, bits, __ATOMIC_RELAXED);
    __atomic_store_n(&g_use.overflow_count, overflow, __ATOMIC_RELAXED);
    __atomic_store_n(&g_use.unbound, unbound, __ATOMIC_RELAXED);
    __atomic_store_n(&g_use.ready,
                     (g_use.count > 0 && unbound == 0) ? 1u : 0u,
                     __ATOMIC_RELEASE);
}

/* -------------------------------------------------------------------------
 * Set / clear / read
 * ------------------------------------------------------------------------- */

error_t UseContextSet(const char *list, bool intern)
{
    UseTag  *tags  = NULL;
    uint32_t count = 0;
    error_t  rc    = list_parse(list, &tags, &count);
    if (rc != OK) return rc;

    if (count == 0) {
        UseContextClear();
        return OK;
    }

    /* Numbers first, outside the lock: the registry takes the volume's door
     * and its own lock, and neither is taken while ours is held. */
    for (uint32_t i = 0; i < count; i++) {
        tags[i].id = intern ? tagfs_tag_intern(tags[i].text)
                            : tagfs_tag_lookup(tags[i].text);
    }

    uint16_t *overflow = kmalloc(sizeof(uint16_t) * count);
    if (!overflow) { tags_free(tags, count); return ERR_NO_MEMORY; }

    spin_lock(&g_use.lock);
    UseTag   *old_tags     = g_use.tags;
    uint32_t  old_count    = g_use.count;
    uint16_t *old_overflow = g_use.overflow;
    g_use.tags     = tags;
    g_use.overflow = overflow;
    __atomic_store_n(&g_use.count, count, __ATOMIC_RELAXED);
    cache_rebuild_locked();
    spin_unlock(&g_use.lock);

    tags_free(old_tags, old_count);
    kfree(old_overflow);

    debug_printf("[USE] set: %u tag(s), %u without a number on this volume\n",
                 count, __atomic_load_n(&g_use.unbound, __ATOMIC_RELAXED));
    return OK;
}

void UseContextClear(void)
{
    spin_lock(&g_use.lock);
    UseTag   *old_tags     = g_use.tags;
    uint32_t  old_count    = g_use.count;
    uint16_t *old_overflow = g_use.overflow;
    g_use.tags     = NULL;
    g_use.overflow = NULL;
    __atomic_store_n(&g_use.count, 0u, __ATOMIC_RELAXED);
    cache_rebuild_locked();
    spin_unlock(&g_use.lock);

    tags_free(old_tags, old_count);
    kfree(old_overflow);
    debug_printf("[USE] cleared\n");
}

size_t UseContextTags(char *buf, size_t cap, uint32_t *out_count)
{
    size_t   need    = 0;
    size_t   used    = 0;
    uint32_t written = 0;

    spin_lock(&g_use.lock);
    for (uint32_t i = 0; i < g_use.count; i++) {
        size_t bytes = strlen(g_use.tags[i].text) + 1;
        need += bytes;
        if (buf && used + bytes <= cap) {
            memcpy(buf + used, g_use.tags[i].text, bytes);
            used += bytes;
            written++;
        }
    }
    spin_unlock(&g_use.lock);

    if (out_count) *out_count = written;
    return need;
}

error_t UseContextTagArray(char **out_block, const char ***out_ptrs, uint32_t *out_count)
{
    *out_block = NULL;
    *out_ptrs  = NULL;
    *out_count = 0;

    for (;;) {
        uint32_t count = 0;
        size_t   need  = UseContextTags(NULL, 0, &count);
        if (need == 0) return OK;

        char *block = kmalloc(need);
        if (!block) return ERR_NO_MEMORY;

        uint32_t got = 0;
        size_t   now = UseContextTags(block, need, &got);
        if (now > need) {
            /* The context grew between the two looks; measure again. */
            kfree(block);
            continue;
        }

        const char **ptrs = kmalloc(sizeof(char *) * (got ? got : 1));
        if (!ptrs) { kfree(block); return ERR_NO_MEMORY; }

        size_t pos = 0;
        for (uint32_t i = 0; i < got; i++) {
            ptrs[i] = block + pos;
            pos += strlen(block + pos) + 1;
        }

        *out_block = block;
        *out_ptrs  = ptrs;
        *out_count = got;
        return OK;
    }
}

bool UseContextIsSet(void)
{
    return __atomic_load_n(&g_use.count, __ATOMIC_RELAXED) > 0;
}

/* -------------------------------------------------------------------------
 * The scheduler's question
 * ------------------------------------------------------------------------- */

bool UseContextMatches(const struct process_t *proc)
{
    if (!proc || !proc->cabin) return false;

    if (!__atomic_load_n(&g_use.ready, __ATOMIC_ACQUIRE)) return false;

    uint64_t bits = __atomic_load_n(&g_use.bits, __ATOMIC_RELAXED);
    uint64_t worn = __atomic_load_n(&proc->cabin->tag_bits, __ATOMIC_RELAXED);
    if ((worn & bits) != bits) return false;

    if (__atomic_load_n(&g_use.overflow_count, __ATOMIC_RELAXED) == 0)
        return true;

    /* Numbers past 63 live in lists on both sides; compare them under the
     * lock, re-reading the fast-path fields because the context may have
     * changed between the loads above and here. */
    spin_lock(&g_use.lock);
    bool match = __atomic_load_n(&g_use.ready, __ATOMIC_RELAXED) != 0 &&
                 (worn & g_use.bits) == g_use.bits;
    if (match) {
        uint16_t  worn_count = __atomic_load_n(&proc->cabin->tag_overflow_count, __ATOMIC_ACQUIRE);
        uint16_t *worn_ids   = __atomic_load_n(&proc->cabin->tag_overflow_ids, __ATOMIC_ACQUIRE);
        for (uint16_t j = 0; j < g_use.overflow_count && match; j++) {
            bool found = false;
            for (uint16_t i = 0; i < worn_count && !found; i++)
                found = worn_ids[i] == g_use.overflow[j];
            match = found;
        }
    }
    spin_unlock(&g_use.lock);
    return match;
}

/* -------------------------------------------------------------------------
 * The volume's numbers
 * ------------------------------------------------------------------------- */

void UseContextRebind(void)
{
    /* Held across the lookups: the registry never calls back in here, so the
     * order use-lock then registry-lock has no reverse, and every lookup is a
     * hash probe under the volume's door — no I/O, no allocation. */
    spin_lock(&g_use.lock);
    for (uint32_t i = 0; i < g_use.count; i++)
        g_use.tags[i].id = tagfs_tag_lookup(g_use.tags[i].text);
    cache_rebuild_locked();
    spin_unlock(&g_use.lock);
}

void UseContextUnbind(void)
{
    spin_lock(&g_use.lock);
    for (uint32_t i = 0; i < g_use.count; i++)
        g_use.tags[i].id = TAGFS_INVALID_TAG_ID;
    cache_rebuild_locked();
    spin_unlock(&g_use.lock);
}

void UseContextBindTag(const char *tag, uint16_t tag_id)
{
    if (!tag || tag_id == TAGFS_INVALID_TAG_ID) return;
    if (__atomic_load_n(&g_use.unbound, __ATOMIC_RELAXED) == 0) return;

    char spelled[USE_TAG_PART_MAX * 2 + 2];
    if (tag_normalise(tag, strlen(tag), spelled, sizeof(spelled)) != TAG_SPELLED) return;

    spin_lock(&g_use.lock);
    for (uint32_t i = 0; i < g_use.count; i++) {
        if (g_use.tags[i].id == TAGFS_INVALID_TAG_ID &&
            strcmp(g_use.tags[i].text, spelled) == 0) {
            g_use.tags[i].id = tag_id;
            cache_rebuild_locked();
            break;
        }
    }
    spin_unlock(&g_use.lock);
}

/* -------------------------------------------------------------------------
 * Lifetime
 * ------------------------------------------------------------------------- */

void UseContextInit(void)
{
    memset(&g_use, 0, sizeof(g_use));
    spinlock_init(&g_use.lock);
}

void UseContextShutdown(void)
{
    UseContextClear();
}
