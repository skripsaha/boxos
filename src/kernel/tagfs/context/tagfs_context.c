#include "tagfs_context.h"
#include "tagfs.h"
#include "klib.h"

#define HASH_TABLE_BUCKETS 64

typedef struct tagfs_context_node {
    uint32_t pid;
    TagFSContext context;
    struct tagfs_context_node* next;
} tagfs_context_node_t;

typedef struct {
    tagfs_context_node_t** buckets;
    size_t bucket_count;
    size_t entry_count;
    spinlock_t lock;
} tagfs_context_table_t;

static tagfs_context_table_t g_context_table;
static bool g_initialized = false;

static inline size_t hash_pid(uint32_t pid, size_t bucket_count) {
    return pid % bucket_count;
}

void tagfs_context_init(void) {
    g_context_table.buckets = (tagfs_context_node_t**)kmalloc(HASH_TABLE_BUCKETS * sizeof(tagfs_context_node_t*));
    if (!g_context_table.buckets) {
        debug_printf("[TagFS Context] FATAL: Failed to allocate hash table buckets\n");
        return;
    }

    memset(g_context_table.buckets, 0, HASH_TABLE_BUCKETS * sizeof(tagfs_context_node_t*));
    g_context_table.bucket_count = HASH_TABLE_BUCKETS;
    g_context_table.entry_count = 0;
    spinlock_init(&g_context_table.lock);

    g_initialized = true;
    debug_printf("[TagFS Context] Initialized with hash table (64 buckets)\n");
}

static TagFSContext* get_or_create_context(uint32_t pid) {
    if (!g_initialized) {
        return NULL;
    }

    size_t bucket_idx = hash_pid(pid, g_context_table.bucket_count);
    tagfs_context_node_t* node = g_context_table.buckets[bucket_idx];

    while (node) {
        if (node->pid == pid) {
            return &node->context;
        }
        node = node->next;
    }

    tagfs_context_node_t* new_node = (tagfs_context_node_t*)kmalloc(sizeof(tagfs_context_node_t));
    if (!new_node) {
        return NULL;
    }

    new_node->pid = pid;
    new_node->context.pid = pid;
    new_node->context.tag_count = 0;
    new_node->context.tag_capacity = TAGFS_CONTEXT_INITIAL_CAPACITY;
    new_node->context.active_tags = kmalloc(sizeof(char*) * TAGFS_CONTEXT_INITIAL_CAPACITY);
    if (!new_node->context.active_tags) {
        kfree(new_node);
        return NULL;
    }
    memset(new_node->context.active_tags, 0, sizeof(char*) * TAGFS_CONTEXT_INITIAL_CAPACITY);

    new_node->next = g_context_table.buckets[bucket_idx];
    g_context_table.buckets[bucket_idx] = new_node;
    g_context_table.entry_count++;

    return &new_node->context;
}

// Grow the active_tags array by 2x
static int context_grow_tags(TagFSContext* ctx) {
    uint32_t new_capacity = ctx->tag_capacity * 2;
    char** new_tags = kmalloc(sizeof(char*) * new_capacity);
    if (!new_tags) {
        return -1;
    }

    memcpy(new_tags, ctx->active_tags, sizeof(char*) * ctx->tag_count);
    memset(new_tags + ctx->tag_count, 0, sizeof(char*) * (new_capacity - ctx->tag_count));
    kfree(ctx->active_tags);
    ctx->active_tags = new_tags;
    ctx->tag_capacity = new_capacity;
    return 0;
}

int tagfs_context_add_tag(uint32_t pid, const char* tag_string) {
    spin_lock(&g_context_table.lock);

    if (!tag_string) {
        spin_unlock(&g_context_table.lock);
        return -1;
    }

    TagFSContext* ctx = get_or_create_context(pid);
    if (!ctx) {
        spin_unlock(&g_context_table.lock);
        return -1;
    }

    // Check for duplicate
    for (uint32_t i = 0; i < ctx->tag_count; i++) {
        if (strcmp(ctx->active_tags[i], tag_string) == 0) {
            spin_unlock(&g_context_table.lock);
            return 0;
        }
    }

    // Grow if needed (no fixed limit)
    if (ctx->tag_count >= ctx->tag_capacity) {
        if (context_grow_tags(ctx) != 0) {
            spin_unlock(&g_context_table.lock);
            return -1;
        }
    }

    // Allocate and copy the tag string
    size_t len = strlen(tag_string);
    ctx->active_tags[ctx->tag_count] = kmalloc(len + 1);
    if (!ctx->active_tags[ctx->tag_count]) {
        spin_unlock(&g_context_table.lock);
        return -1;
    }
    memcpy(ctx->active_tags[ctx->tag_count], tag_string, len);
    ctx->active_tags[ctx->tag_count][len] = '\0';
    ctx->tag_count++;

    spin_unlock(&g_context_table.lock);
    return 0;
}

int tagfs_context_remove_tag(uint32_t pid, const char* tag_string) {
    spin_lock(&g_context_table.lock);

    if (!tag_string) {
        spin_unlock(&g_context_table.lock);
        return -1;
    }

    TagFSContext* ctx = get_or_create_context(pid);
    if (!ctx) {
        spin_unlock(&g_context_table.lock);
        return -1;
    }

    for (uint32_t i = 0; i < ctx->tag_count; i++) {
        if (strcmp(ctx->active_tags[i], tag_string) == 0) {
            kfree(ctx->active_tags[i]);
            // Shift remaining tags
            for (uint32_t j = i; j < ctx->tag_count - 1; j++) {
                ctx->active_tags[j] = ctx->active_tags[j + 1];
            }
            ctx->active_tags[ctx->tag_count - 1] = NULL;
            ctx->tag_count--;
            spin_unlock(&g_context_table.lock);
            return 0;
        }
    }

    spin_unlock(&g_context_table.lock);
    return -1;
}

void tagfs_context_clear(uint32_t pid) {
    spin_lock(&g_context_table.lock);

    TagFSContext* ctx = get_or_create_context(pid);
    if (ctx) {
        for (uint32_t i = 0; i < ctx->tag_count; i++) {
            if (ctx->active_tags[i]) {
                kfree(ctx->active_tags[i]);
                ctx->active_tags[i] = NULL;
            }
        }
        ctx->tag_count = 0;
    }

    spin_unlock(&g_context_table.lock);
}

bool tagfs_context_matches(uint32_t pid, uint32_t file_id) {
    spin_lock(&g_context_table.lock);

    TagFSContext* ctx = get_or_create_context(pid);
    if (!ctx || ctx->tag_count == 0) {
        spin_unlock(&g_context_table.lock);
        return true;
    }

    TagFSMetadata* meta = tagfs_get_metadata(file_id);
    if (!meta || !(meta->flags & TAGFS_FILE_ACTIVE)) {
        spin_unlock(&g_context_table.lock);
        return false;
    }

    for (uint32_t ctx_idx = 0; ctx_idx < ctx->tag_count; ctx_idx++) {
        bool found = false;

        const char* ctx_tag = ctx->active_tags[ctx_idx];

        for (uint32_t tag_idx = 0; tag_idx < meta->tag_count; tag_idx++) {
            char tag_string[64];
            tagfs_format_tag(tag_string, meta->tags[tag_idx].key, meta->tags[tag_idx].value);

            if (tag_match(ctx_tag, tag_string)) {
                found = true;
                break;
            }
        }

        if (!found) {
            spin_unlock(&g_context_table.lock);
            return false;
        }
    }

    spin_unlock(&g_context_table.lock);
    return true;
}

int tagfs_context_get_tags(uint32_t pid, const char* tags[], uint32_t max_tags) {
    spin_lock(&g_context_table.lock);

    TagFSContext* ctx = get_or_create_context(pid);
    if (!ctx || ctx->tag_count == 0) {
        spin_unlock(&g_context_table.lock);
        return 0;
    }

    uint32_t count = ctx->tag_count < max_tags ? ctx->tag_count : max_tags;
    for (uint32_t i = 0; i < count; i++) {
        tags[i] = ctx->active_tags[i];
    }

    spin_unlock(&g_context_table.lock);
    return count;
}

void tagfs_context_destroy(uint32_t pid) {
    spin_lock(&g_context_table.lock);

    if (!g_initialized) {
        spin_unlock(&g_context_table.lock);
        return;
    }

    size_t bucket_idx = hash_pid(pid, g_context_table.bucket_count);
    tagfs_context_node_t** node_ptr = &g_context_table.buckets[bucket_idx];
    tagfs_context_node_t* node = *node_ptr;

    while (node) {
        if (node->pid == pid) {
            *node_ptr = node->next;
            // Free all tag strings
            for (uint32_t i = 0; i < node->context.tag_count; i++) {
                if (node->context.active_tags[i]) {
                    kfree(node->context.active_tags[i]);
                }
            }
            if (node->context.active_tags) {
                kfree(node->context.active_tags);
            }
            kfree(node);
            g_context_table.entry_count--;
            spin_unlock(&g_context_table.lock);
            return;
        }
        node_ptr = &node->next;
        node = node->next;
    }

    spin_unlock(&g_context_table.lock);
}
