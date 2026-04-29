#include "op_registry.h"
#include "klib.h"

/*
 * Hash-chained registry. Each bucket holds a singly-linked list of
 * OpRegistryNode. Resize doubles bucket count when load factor exceeds 75%.
 *
 * Bucket count is always a power of two; index = mix(op_kind) & (cap - 1).
 *
 * The table is owned by a single global OpRegistryState. Concurrency is
 * protected by a spinlock for the rare write paths (register, resize) and
 * by stable-pointer reads for the hot path (lookup).
 *
 * Memory ownership:
 *   - Each OpRegistryNode is kmalloc'd on register and lives until shutdown.
 *   - The buckets array is kmalloc'd and reallocated on resize.
 *   - name is pointer-only; caller guarantees lifetime ≥ shutdown.
 */

#define OP_REGISTRY_INITIAL_BUCKETS 64u
#define OP_REGISTRY_MAX_BUCKETS     (1u << 20)
#define OP_REGISTRY_LOAD_NUM        3
#define OP_REGISTRY_LOAD_DEN        4

typedef struct OpRegistryNode {
    OpRegistration         entry;
    struct OpRegistryNode *next;
} OpRegistryNode;

typedef struct {
    OpRegistryNode **buckets;
    uint32_t         bucket_count;
    uint32_t         entry_count;
    spinlock_t       lock;
    bool             initialized;
} OpRegistryState;

static OpRegistryState g_op_registry;

static inline uint32_t op_kind_hash(uint32_t op_kind)
{
    /* Mix high (deck) and low (opcode) halves. Knuth multiplicative hash. */
    uint32_t k = op_kind ^ (op_kind >> 16);
    return k * 2654435761u;
}

static inline uint32_t bucket_of(uint32_t op_kind, uint32_t bucket_count)
{
    return op_kind_hash(op_kind) & (bucket_count - 1u);
}

static error_t op_registry_resize_locked(uint32_t new_count)
{
    if (new_count < OP_REGISTRY_INITIAL_BUCKETS) new_count = OP_REGISTRY_INITIAL_BUCKETS;
    if (new_count > OP_REGISTRY_MAX_BUCKETS)     new_count = OP_REGISTRY_MAX_BUCKETS;
    if ((new_count & (new_count - 1u)) != 0u)    return ERR_INVALID_ARGUMENT;
    if (new_count == g_op_registry.bucket_count) return OK;

    OpRegistryNode **new_buckets = kmalloc(sizeof(OpRegistryNode *) * new_count);
    if (!new_buckets) return ERR_NO_MEMORY;
    for (uint32_t i = 0; i < new_count; i++) new_buckets[i] = NULL;

    /* Rehash every node into the new bucket array. */
    for (uint32_t b = 0; b < g_op_registry.bucket_count; b++) {
        OpRegistryNode *n = g_op_registry.buckets[b];
        while (n) {
            OpRegistryNode *next = n->next;
            uint32_t new_b = bucket_of(n->entry.op_kind, new_count);
            n->next = new_buckets[new_b];
            new_buckets[new_b] = n;
            n = next;
        }
    }

    kfree(g_op_registry.buckets);
    g_op_registry.buckets      = new_buckets;
    g_op_registry.bucket_count = new_count;
    return OK;
}

static bool op_registry_should_grow(void)
{
    /* entry_count / bucket_count > LOAD_NUM / LOAD_DEN  ↔  entries*DEN > buckets*NUM */
    return (uint64_t)g_op_registry.entry_count * (uint64_t)OP_REGISTRY_LOAD_DEN >
           (uint64_t)g_op_registry.bucket_count * (uint64_t)OP_REGISTRY_LOAD_NUM;
}

error_t OpRegistryInit(void)
{
    if (g_op_registry.initialized) return ERR_ALREADY_INITIALIZED;

    spinlock_init(&g_op_registry.lock);
    g_op_registry.bucket_count = OP_REGISTRY_INITIAL_BUCKETS;
    g_op_registry.entry_count  = 0;
    g_op_registry.buckets = kmalloc(sizeof(OpRegistryNode *) * OP_REGISTRY_INITIAL_BUCKETS);
    if (!g_op_registry.buckets) return ERR_NO_MEMORY;
    for (uint32_t i = 0; i < OP_REGISTRY_INITIAL_BUCKETS; i++) g_op_registry.buckets[i] = NULL;

    g_op_registry.initialized = true;
    debug_printf("[OpRegistry] initialized (initial buckets=%u)\n", OP_REGISTRY_INITIAL_BUCKETS);
    return OK;
}

void OpRegistryShutdown(void)
{
    if (!g_op_registry.initialized) return;

    spin_lock(&g_op_registry.lock);
    for (uint32_t b = 0; b < g_op_registry.bucket_count; b++) {
        OpRegistryNode *n = g_op_registry.buckets[b];
        while (n) {
            OpRegistryNode *next = n->next;
            kfree(n);
            n = next;
        }
    }
    kfree(g_op_registry.buckets);
    g_op_registry.buckets      = NULL;
    g_op_registry.bucket_count = 0;
    g_op_registry.entry_count  = 0;
    g_op_registry.initialized  = false;
    spin_unlock(&g_op_registry.lock);
}

error_t OpRegistryRegister(uint32_t    op_kind,
                           OpHandler   handler,
                           uint32_t    security_mask,
                           const char *name)
{
    if (!g_op_registry.initialized) return ERR_NOT_INITIALIZED;
    if (!handler)                   return ERR_NULL_POINTER;

    spin_lock(&g_op_registry.lock);

    /* Reject duplicates. */
    uint32_t b = bucket_of(op_kind, g_op_registry.bucket_count);
    for (OpRegistryNode *n = g_op_registry.buckets[b]; n; n = n->next) {
        if (n->entry.op_kind == op_kind) {
            spin_unlock(&g_op_registry.lock);
            return ERR_ALREADY_EXISTS;
        }
    }

    OpRegistryNode *node = kmalloc(sizeof(OpRegistryNode));
    if (!node) {
        spin_unlock(&g_op_registry.lock);
        return ERR_NO_MEMORY;
    }
    node->entry.op_kind       = op_kind;
    node->entry.security_mask = security_mask;
    node->entry.handler       = handler;
    node->entry.name          = name;

    node->next = g_op_registry.buckets[b];
    g_op_registry.buckets[b] = node;
    g_op_registry.entry_count++;

    if (op_registry_should_grow()) {
        uint32_t new_cap = g_op_registry.bucket_count * 2u;
        error_t  rc      = op_registry_resize_locked(new_cap);
        if (rc != OK) {
            /* Resize failure is non-fatal; the table just stays denser. */
            debug_printf("[OpRegistry] resize to %u failed (%d), continuing\n", new_cap, rc);
        }
    }

    spin_unlock(&g_op_registry.lock);
    return OK;
}

const OpRegistration *OpRegistryLookup(uint32_t op_kind)
{
    if (!g_op_registry.initialized) return NULL;

    /* Read-only fast path; we still take the lock to be safe under concurrent
     * resize. A future optimization can move to RCU semantics with a stable
     * snapshot pointer; for now correctness over micro-optimization.
     */
    spin_lock(&g_op_registry.lock);
    uint32_t b = bucket_of(op_kind, g_op_registry.bucket_count);
    const OpRegistration *result = NULL;
    for (OpRegistryNode *n = g_op_registry.buckets[b]; n; n = n->next) {
        if (n->entry.op_kind == op_kind) {
            result = &n->entry;
            break;
        }
    }
    spin_unlock(&g_op_registry.lock);
    return result;
}

uint32_t OpRegistryCount(void)
{
    if (!g_op_registry.initialized) return 0;
    return g_op_registry.entry_count;
}

void OpRegistryDump(void)
{
    if (!g_op_registry.initialized) {
        kprintf("[OpRegistry] not initialized\n");
        return;
    }
    spin_lock(&g_op_registry.lock);
    kprintf("[OpRegistry] %u entries in %u buckets:\n",
            g_op_registry.entry_count, g_op_registry.bucket_count);
    for (uint32_t b = 0; b < g_op_registry.bucket_count; b++) {
        for (OpRegistryNode *n = g_op_registry.buckets[b]; n; n = n->next) {
            kprintf("  [%u] op_kind=0x%08x deck=%u op=%u name=%s\n",
                    b, n->entry.op_kind,
                    OP_KIND_DECK(n->entry.op_kind), OP_KIND_OPCODE(n->entry.op_kind),
                    n->entry.name ? n->entry.name : "(unnamed)");
        }
    }
    spin_unlock(&g_op_registry.lock);
}
