#include "tagfs_context.h"
#include "../tag_bitmap/tag_bitmap.h"
#include "../tag_registry/tag_registry.h"

#define CONTEXT_BUCKETS 64
#define OVERFLOW_MIN_CAPACITY 4

typedef struct ContextNode
{
    TagFSContext context;
    struct ContextNode *next;
} ContextNode;

static ContextNode *g_buckets[CONTEXT_BUCKETS];
static bool g_initialized = false;
static spinlock_t g_lock;

static inline uint32_t ctx_hash(uint32_t pid)
{
    return pid % CONTEXT_BUCKETS;
}

void tagfs_context_init(void)
{
    memset(g_buckets, 0, sizeof(g_buckets));
    spinlock_init(&g_lock);
    g_initialized = true;
    debug_printf("[TagFS Context] Initialized (%d buckets)\n", CONTEXT_BUCKETS);
}

static TagFSContext *find_context(uint32_t pid)
{
    ContextNode *node = g_buckets[ctx_hash(pid)];
    while (node)
    {
        if (node->context.pid == pid)
            return &node->context;
        node = node->next;
    }
    return NULL;
}

static TagFSContext *find_or_create_context(uint32_t pid)
{
    uint32_t bucket = ctx_hash(pid);
    ContextNode *node = g_buckets[bucket];
    while (node)
    {
        if (node->context.pid == pid)
            return &node->context;
        node = node->next;
    }

    ContextNode *new_node = (ContextNode *)kmalloc(sizeof(ContextNode));
    if (!new_node)
        return NULL;

    memset(new_node, 0, sizeof(ContextNode));
    new_node->context.pid = pid;
    new_node->context.context_bits = 0;
    new_node->context.overflow_ids = NULL;
    new_node->context.overflow_count = 0;
    new_node->context.overflow_capacity = 0;

    new_node->next = g_buckets[bucket];
    g_buckets[bucket] = new_node;
    return &new_node->context;
}

int tagfs_context_add_tag(uint32_t pid, uint16_t tag_id)
{
    spin_lock(&g_lock);

    TagFSContext *ctx = find_or_create_context(pid);
    if (!ctx)
    {
        spin_unlock(&g_lock);
        return -1;
    }

    if (tag_id < 64)
    {
        ctx->context_bits |= (uint64_t)1 << tag_id;
        spin_unlock(&g_lock);
        return 0;
    }

    for (uint16_t i = 0; i < ctx->overflow_count; i++)
    {
        if (ctx->overflow_ids[i] == tag_id)
        {
            spin_unlock(&g_lock);
            return 0;
        }
    }

    if (ctx->overflow_count >= ctx->overflow_capacity)
    {
        uint16_t new_cap = ctx->overflow_capacity == 0
                               ? OVERFLOW_MIN_CAPACITY
                               : ctx->overflow_capacity * 2;
        uint16_t *grown = (uint16_t *)kmalloc(new_cap * sizeof(uint16_t));
        if (!grown)
        {
            spin_unlock(&g_lock);
            return -1;
        }
        if (ctx->overflow_ids)
        {
            memcpy(grown, ctx->overflow_ids, ctx->overflow_count * sizeof(uint16_t));
            kfree(ctx->overflow_ids);
        }
        ctx->overflow_ids = grown;
        ctx->overflow_capacity = new_cap;
    }

    ctx->overflow_ids[ctx->overflow_count++] = tag_id;
    spin_unlock(&g_lock);
    return 0;
}

int tagfs_context_add_tag_string(uint32_t pid, const char *key, const char *value)
{
    TagFSState *state = tagfs_get_state();
    if (!state || !state->registry)
        return -1;

    uint16_t tag_id = tag_registry_intern(state->registry, key, value);
    if (tag_id == TAGFS_INVALID_TAG_ID)
        return -1;

    return tagfs_context_add_tag(pid, tag_id);
}

int tagfs_context_remove_tag(uint32_t pid, uint16_t tag_id)
{
    spin_lock(&g_lock);

    TagFSContext *ctx = find_context(pid);
    if (!ctx)
    {
        spin_unlock(&g_lock);
        return -1;
    }

    if (tag_id < 64)
    {
        ctx->context_bits &= ~((uint64_t)1 << tag_id);
        spin_unlock(&g_lock);
        return 0;
    }

    for (uint16_t i = 0; i < ctx->overflow_count; i++)
    {
        if (ctx->overflow_ids[i] == tag_id)
        {
            ctx->overflow_ids[i] = ctx->overflow_ids[--ctx->overflow_count];
            spin_unlock(&g_lock);
            return 0;
        }
    }

    spin_unlock(&g_lock);
    return -1;
}

void tagfs_context_clear(uint32_t pid)
{
    spin_lock(&g_lock);

    TagFSContext *ctx = find_context(pid);
    if (ctx)
    {
        ctx->context_bits = 0;
        if (ctx->overflow_ids)
        {
            kfree(ctx->overflow_ids);
            ctx->overflow_ids = NULL;
            ctx->overflow_count = 0;
            ctx->overflow_capacity = 0;
        }
    }

    spin_unlock(&g_lock);
}

bool tagfs_context_matches_file(uint32_t pid, uint32_t file_id)
{
    uint64_t ctx_bits;
    uint16_t overflow_count;
    uint16_t *overflow_snapshot = NULL;

    spin_lock(&g_lock);

    TagFSContext *ctx = find_context(pid);
    if (!ctx || (ctx->context_bits == 0 && ctx->overflow_count == 0))
    {
        spin_unlock(&g_lock);
        return true;
    }

    ctx_bits = ctx->context_bits;
    overflow_count = ctx->overflow_count;
    if (overflow_count > 0)
    {
        overflow_snapshot = kmalloc(overflow_count * sizeof(uint16_t));
        if (overflow_snapshot)
        {
            memcpy(overflow_snapshot, ctx->overflow_ids, overflow_count * sizeof(uint16_t));
        }
        else
        {
            overflow_count = 0;
        }
    }

    spin_unlock(&g_lock);

    // All bitmap queries run lock-free from here
    TagFSState *state = tagfs_get_state();
    if (!state || !state->bitmap_index)
    {
        return false;
    }

    int tag_count = tag_bitmap_tag_count_for_file(state->bitmap_index, file_id);
    if (tag_count <= 0)
    {
        return (ctx_bits == 0 && overflow_count == 0);
    }

    uint16_t *file_tag_ids = kmalloc(sizeof(uint16_t) * (uint32_t)tag_count);
    if (!file_tag_ids)
    {
        return false;
    }

    int count = tag_bitmap_tags_for_file(state->bitmap_index, file_id,
                                         file_tag_ids, (uint32_t)tag_count);
    if (count < 0)
        count = 0;

    uint64_t file_bits = 0;
    for (int i = 0; i < count; i++)
    {
        if (file_tag_ids[i] < 64)
            file_bits |= (uint64_t)1 << file_tag_ids[i];
    }

    if ((file_bits & ctx_bits) != ctx_bits)
    {
        kfree(file_tag_ids);
        if (overflow_snapshot)
            kfree(overflow_snapshot);
        return false;
    }

    for (uint16_t oi = 0; oi < overflow_count; oi++)
    {
        bool found = false;
        for (int fi = 0; fi < count; fi++)
        {
            if (file_tag_ids[fi] == overflow_snapshot[oi])
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            kfree(file_tag_ids);
            if (overflow_snapshot)
                kfree(overflow_snapshot);
            return false;
        }
    }

    kfree(file_tag_ids);
    if (overflow_snapshot)
        kfree(overflow_snapshot);
    return true;
}

void tagfs_context_destroy(uint32_t pid)
{
    spin_lock(&g_lock);

    uint32_t bucket = ctx_hash(pid);
    ContextNode **ptr = &g_buckets[bucket];
    ContextNode *node = *ptr;

    while (node)
    {
        if (node->context.pid == pid)
        {
            *ptr = node->next;
            if (node->context.overflow_ids)
                kfree(node->context.overflow_ids);
            kfree(node);
            spin_unlock(&g_lock);
            return;
        }
        ptr = &node->next;
        node = node->next;
    }

    spin_unlock(&g_lock);
}

uint64_t tagfs_context_get_bits(uint32_t pid)
{
    spin_lock(&g_lock);

    TagFSContext *ctx = find_context(pid);
    uint64_t bits = ctx ? ctx->context_bits : 0;

    spin_unlock(&g_lock);
    return bits;
}

int tagfs_context_get_tags(uint32_t pid, const char *tags[], uint32_t max_tags)
{
    spin_lock(&g_lock);

    TagFSContext *ctx = find_context(pid);
    if (!ctx || max_tags == 0)
    {
        spin_unlock(&g_lock);
        return 0;
    }

    TagFSState *state = tagfs_get_state();
    TagRegistry *reg = state ? state->registry : NULL;

    uint32_t count = 0;

    // FIX: Return full tags (key:value) not just keys.
    // Caller must provide buffer space for each tag string.
    // We use static buffers per slot — caller should use tags immediately.
    static char tag_buffers[64][64]; // 64 tags max, 64 chars each

    for (uint16_t bit = 0; bit < 64 && count < max_tags; bit++)
    {
        if (!(ctx->context_bits & ((uint64_t)1 << bit)))
            continue;

        const char *key = reg ? tag_registry_key(reg, bit) : NULL;
        const char *value = reg ? tag_registry_value(reg, bit) : NULL;

        if (key)
        {
            tagfs_format_tag(tag_buffers[count], sizeof(tag_buffers[count]), key, value);
            tags[count] = tag_buffers[count];
            count++;
        }
    }

    for (uint16_t oi = 0; oi < ctx->overflow_count && count < max_tags; oi++)
    {
        uint16_t tid = ctx->overflow_ids[oi];
        const char *key = reg ? tag_registry_key(reg, tid) : NULL;
        const char *value = reg ? tag_registry_value(reg, tid) : NULL;

        if (key)
        {
            tagfs_format_tag(tag_buffers[count], sizeof(tag_buffers[count]), key, value);
            tags[count] = tag_buffers[count];
            count++;
        }
    }

    spin_unlock(&g_lock);
    return (int)count;
}
