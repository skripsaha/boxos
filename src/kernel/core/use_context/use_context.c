#include "use_context.h"
#include "tagfs.h"
#include "tag_registry.h"
#include "klib.h"
#include "atomics.h"

// ============================================================================
// Global State
// ============================================================================

static UseContext g_use_context;
static spinlock_t g_context_lock;

// ============================================================================
// Public API
// ============================================================================

void UseContextInit(void) {
    memset(&g_use_context, 0, sizeof(UseContext));
    spinlock_init(&g_context_lock);
}

void UseContextShutdown(void) {
    spin_lock(&g_context_lock);
    if (g_use_context.overflow_ids) {
        kfree(g_use_context.overflow_ids);
        g_use_context.overflow_ids = NULL;
    }
    g_use_context.enabled = false;
    spin_unlock(&g_context_lock);
}

error_t UseContextSet(const char *tags[], uint32_t count) {
    if (count == 0) {
        UseContextClear();
        return OK;
    }

    if (!tags) {
        return ERR_INVALID_ARGUMENT;
    }

    TagFSState *fs = tagfs_get_state();
    if (!fs || !fs->registry) {
        return ERR_NOT_INITIALIZED;
    }

    // Resolve all tag IDs BEFORE acquiring g_context_lock
    // This prevents lock ordering violation: g_context_lock -> reg->lock
    uint16_t resolved_tags[64];
    uint32_t resolved_count = 0;

    for (uint32_t i = 0; i < count && resolved_count < 64; i++) {
        if (!tags[i]) {
            continue;
        }

        char key[256], value[256];
        tagfs_parse_tag(tags[i], key, sizeof(key), value, sizeof(value));

        uint16_t tid = tag_registry_intern(fs->registry, key, value[0] ? value : NULL);
        if (tid != TAGFS_INVALID_TAG_ID) {
            resolved_tags[resolved_count++] = tid;
        }
    }

    // Now acquire g_context_lock and update context (no nested locks)
    spin_lock(&g_context_lock);

    g_use_context.context_bits = 0;
    g_use_context.overflow_count = 0;

    for (uint32_t i = 0; i < resolved_count; i++) {
        uint16_t tid = resolved_tags[i];

        if (tid < 64) {
            g_use_context.context_bits |= ((uint64_t)1 << tid);
        } else {
            if (g_use_context.overflow_count >= g_use_context.overflow_capacity) {
                uint16_t new_cap = g_use_context.overflow_capacity == 0
                                       ? 8
                                       : g_use_context.overflow_capacity * 2;
                uint16_t *new_ids = kmalloc(sizeof(uint16_t) * new_cap);
                if (new_ids) {
                    if (g_use_context.overflow_ids) {
                        memcpy(new_ids, g_use_context.overflow_ids,
                               sizeof(uint16_t) * g_use_context.overflow_count);
                        kfree(g_use_context.overflow_ids);
                    }
                    g_use_context.overflow_ids = new_ids;
                    g_use_context.overflow_capacity = new_cap;
                }
            }
            if (g_use_context.overflow_count < g_use_context.overflow_capacity) {
                g_use_context.overflow_ids[g_use_context.overflow_count++] = tid;
            }
        }
    }

    g_use_context.enabled = true;
    spin_unlock(&g_context_lock);

    debug_printf("[USE_CTX] Set: %u tags, bits=0x%lx overflow=%u\n",
                  resolved_count, g_use_context.context_bits, g_use_context.overflow_count);

    return OK;
}

void UseContextClear(void) {
    spin_lock(&g_context_lock);
    g_use_context.enabled = false;
    g_use_context.context_bits = 0;
    if (g_use_context.overflow_ids) {
        kfree(g_use_context.overflow_ids);
        g_use_context.overflow_ids = NULL;
    }
    g_use_context.overflow_count = 0;
    g_use_context.overflow_capacity = 0;
    spin_unlock(&g_context_lock);
    debug_printf("[USE_CTX] Cleared\n");
}

bool UseContextMatches(const process_t *proc) {
    if (!proc) {
        return false;
    }

    // Fast path: lock-free check for common case (no overflow tags)
    if (!__atomic_load_n(&g_use_context.enabled, __ATOMIC_RELAXED)) {
        return false;
    }

    uint64_t ctx_bits = __atomic_load_n(&g_use_context.context_bits, __ATOMIC_RELAXED);
    if (ctx_bits && (proc->tag_bits & ctx_bits) != ctx_bits) {
        return false;
    }

    // If no overflow tags, match is confirmed without lock
    uint16_t overflow_count = __atomic_load_n(&g_use_context.overflow_count, __ATOMIC_RELAXED);
    if (overflow_count == 0) {
        return true;
    }

    // Slow path: only lock for overflow tag check
    spin_lock(&g_context_lock);

    // Re-check under lock (context may have changed)
    if (!g_use_context.enabled) {
        spin_unlock(&g_context_lock);
        return false;
    }

    ctx_bits = g_use_context.context_bits;
    if (ctx_bits && (proc->tag_bits & ctx_bits) != ctx_bits) {
        spin_unlock(&g_context_lock);
        return false;
    }

    overflow_count = g_use_context.overflow_count;
    uint16_t *ctx_overflow_ids = g_use_context.overflow_ids;
    uint16_t proc_overflow_count = __atomic_load_n(&proc->tag_overflow_count, __ATOMIC_ACQUIRE);
    uint16_t *proc_overflow_ids = __atomic_load_n(&proc->tag_overflow_ids, __ATOMIC_ACQUIRE);

    bool match = true;
    for (uint16_t j = 0; j < overflow_count && match; j++) {
        bool found = false;
        for (uint16_t i = 0; i < proc_overflow_count && !found; i++) {
            if (proc_overflow_ids[i] == ctx_overflow_ids[j]) {
                found = true;
            }
        }
        if (!found) {
            match = false;
        }
    }

    spin_unlock(&g_context_lock);
    return match;
}

const UseContext *UseContextGet(void) {
    return &g_use_context;
}
