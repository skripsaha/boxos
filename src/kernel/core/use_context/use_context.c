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

/* -------------------------------------------------------------------------
 * Names
 *
 * A context tag is kept in its canonical text, "key" or "key:value", as the
 * one spelling (tag_spelling.h) reads it — so a tag the volume registry could
 * not hold cannot be a context tag either, and two ways of typing one tag are
 * one tag here.
 * ------------------------------------------------------------------------- */

static void tags_free(UseTag *tags, uint32_t count)
{
    if (!tags) return;
    for (uint32_t i = 0; i < count; i++) kfree(tags[i].text);
    kfree(tags);
}

/*
 * Parse a comma-separated list into an array of unbound tags, each in its
 * canonical text. The whole list is spelled before anything is allocated, and
 * a tag that does not spell refuses it with the spelling's code. A duplicate
 * is kept once. On any failure nothing is kept and the error is the caller's
 * to report.
 */
static error_t list_parse(const char *list, UseTag **out_tags, uint32_t *out_count)
{
    *out_tags  = NULL;
    *out_count = 0;

    size_t   len    = list ? strlen(list) : 0;
    uint32_t spoken = 0;
    error_t  rc     = TagListCount(list, len, &spoken);
    if (rc != OK || spoken == 0) return rc;

    UseTag *tags = kmalloc(sizeof(UseTag) * spoken);
    if (!tags) return ERR_NO_MEMORY;

    uint32_t count = 0;
    size_t   at    = 0;
    for (uint32_t k = 0; k < spoken; k++) {
        TagSpelling tag;
        (void)TagListNext(list, len, &at, &tag);   /* judged whole above */

        bool seen = false;
        for (uint32_t i = 0; i < count && !seen; i++)
            seen = TagSpellingMatchesText(&tag, tags[i].text);
        if (seen) continue;

        char *text = kmalloc(TagSpellingTextLength(&tag) + 1);
        if (!text) { tags_free(tags, count); return ERR_NO_MEMORY; }
        TagSpellingText(&tag, text);
        tags[count].text = text;
        tags[count].id   = TAGFS_INVALID_TAG_ID;
        count++;
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
        if (!intern)
            tags[i].id = tagfs_tag_lookup(tags[i].text);
        else if (tagfs_tag_intern(tags[i].text, &tags[i].id) != OK)
            tags[i].id = TAGFS_INVALID_TAG_ID;
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
        uint32_t  worn_count = __atomic_load_n(&proc->cabin->tag_overflow_count, __ATOMIC_ACQUIRE);
        uint16_t *worn_ids   = __atomic_load_n(&proc->cabin->tag_overflow_ids, __ATOMIC_ACQUIRE);
        for (uint16_t j = 0; j < g_use.overflow_count && match; j++) {
            bool found = false;
            for (uint32_t i = 0; i < worn_count && !found; i++)
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

/* The volume's book changed under the cache: look every tag up in the registry
 * that was just mounted — lookup only, because mounting somebody's medium must
 * not write into it. */
static void UseContextRebind(void)
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

void UseContextRemember(bool *remembered)
{
    bool kept = false;
    if (remembered) *remembered = false;

    /* Through the door, like every caller from outside the volume: the ground
     * may be clearing under a medium that just left. */
    if (!tagfs_enter()) return;

    uint32_t n    = 0;
    size_t   need = UseContextTags(NULL, 0, &n);
    char    *block = need ? kmalloc(need) : NULL;
    if (need == 0 || block) {
        size_t len = 0;
        if (need) {
            /* NUL-separated to comma-joined, no NUL at the end — the form
             * UseContextSet reads back. */
            uint32_t got = 0;
            UseContextTags(block, need, &got);
            size_t pos = 0;
            for (uint32_t i = 0; i + 1 < got; i++) {
                pos += strlen(block + pos);
                block[pos++] = ',';
            }
            len = got ? pos + strlen(block + pos) : 0;
        }
        tagfs_remember_use_context(block, len, &kept);
        kfree(block);
    }
    tagfs_leave();

    if (remembered) *remembered = kept;
}

void UseContextRecall(void)
{
    if (UseContextIsSet()) {
        UseContextRebind();
        bool kept = false;
        UseContextRemember(&kept);
        if (!kept)
            kprintf("[USE] the context stands, and this volume will not remember it\n");
        return;
    }

    size_t need = tagfs_recall_use_context(NULL, 0);
    if (need == 0) return;
    char *list = kmalloc(need + 1);
    if (!list) return;
    tagfs_recall_use_context(list, need);
    list[need] = '\0';

    error_t rc = UseContextSet(list, false);
    if (rc == OK)
        kprintf("[USE] the volume remembers the Use Context: %s\n", list);
    else
        kprintf("[USE] the volume remembers a Use Context this kernel cannot spell "
                "(error %d): %s\n", (int)rc, list);
    kfree(list);
}

void UseContextUnbind(void)
{
    spin_lock(&g_use.lock);
    for (uint32_t i = 0; i < g_use.count; i++)
        g_use.tags[i].id = TAGFS_INVALID_TAG_ID;
    cache_rebuild_locked();
    spin_unlock(&g_use.lock);
}

void UseContextBindTag(const TagSpelling *tag, uint16_t tag_id)
{
    if (!tag || tag_id == TAGFS_INVALID_TAG_ID) return;
    if (__atomic_load_n(&g_use.unbound, __ATOMIC_RELAXED) == 0) return;

    spin_lock(&g_use.lock);
    for (uint32_t i = 0; i < g_use.count; i++) {
        if (g_use.tags[i].id == TAGFS_INVALID_TAG_ID &&
            TagSpellingMatchesText(tag, g_use.tags[i].text)) {
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
