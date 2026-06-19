#include "manifest.h"
#include "manifest_auth.h"
#include "atomics.h"
#include "process.h"
#include "vmm.h"

/*
 * Global ManifestTable: a growable slot array plus per-slot generation
 * counters. Handles are encoded as (gen<<32 | slot). Stale handles fail
 * lookup because the slot's generation has advanced.
 *
 * Free slots form a chain rooted at next_free; on alloc we pop, on release
 * we push. When the chain is empty, the table doubles in size.
 *
 * All structural mutation goes through table.lock. Per-slot ref_count is
 * atomic so hot ManifestResolve / ManifestRelease paths don't serialize.
 */

#define MANIFEST_TABLE_INITIAL_CAP 32u
#define MANIFEST_TABLE_MAX_CAP     (1u << 24)

/* Upper bound on raw Manifest size — shared with guide.c dispatch staging
 * via MANIFEST_RAW_MAX_SIZE in manifest.h. */

typedef struct ManifestSlot {
    CompiledManifest *manifest;     /* NULL when slot is free */
    uint32_t          generation;   /* incremented every release */
    uint32_t          next_free;    /* index of next free slot, or 0xFFFFFFFF */
} ManifestSlot;

typedef struct {
    ManifestSlot *slots;
    uint32_t      capacity;
    uint32_t      first_free;
    uint32_t      active_count;
    spinlock_t    lock;
    bool          initialized;
} ManifestTable;

#define MANIFEST_SLOT_NIL 0xFFFFFFFFu

static ManifestTable g_manifest_table;

/* --------------------------------------------------------------------------
 * Slot management
 * -------------------------------------------------------------------------- */

static void slot_init_range(ManifestSlot *slots, uint32_t from, uint32_t to)
{
    /* Build a free-list chain over [from, to). */
    for (uint32_t i = from; i < to; i++) {
        slots[i].manifest   = NULL;
        slots[i].generation = 1;  /* start at 1 so handle (gen=0) is always invalid */
        slots[i].next_free  = (i + 1 < to) ? (i + 1) : MANIFEST_SLOT_NIL;
    }
}

static error_t manifest_table_grow_locked(uint32_t new_cap)
{
    if (new_cap < g_manifest_table.capacity) return ERR_INVALID_ARGUMENT;
    if (new_cap > MANIFEST_TABLE_MAX_CAP)    return ERR_NO_MEMORY;
    if (new_cap == g_manifest_table.capacity) return OK;

    ManifestSlot *new_slots = kmalloc(sizeof(ManifestSlot) * new_cap);
    if (!new_slots) return ERR_NO_MEMORY;

    if (g_manifest_table.slots) {
        for (uint32_t i = 0; i < g_manifest_table.capacity; i++) {
            new_slots[i] = g_manifest_table.slots[i];
        }
    }

    /* Append new free slots to the free list. New range is appended in order
     * and linked into the existing chain. */
    uint32_t old_cap = g_manifest_table.capacity;
    slot_init_range(new_slots, old_cap, new_cap);

    /* Find tail of existing free list and link to new range, or set head. */
    if (g_manifest_table.first_free == MANIFEST_SLOT_NIL) {
        g_manifest_table.first_free = old_cap;
    } else {
        uint32_t cur = g_manifest_table.first_free;
        while (new_slots[cur].next_free != MANIFEST_SLOT_NIL) cur = new_slots[cur].next_free;
        new_slots[cur].next_free = old_cap;
    }

    if (g_manifest_table.slots) kfree(g_manifest_table.slots);
    g_manifest_table.slots    = new_slots;
    g_manifest_table.capacity = new_cap;
    return OK;
}

static error_t manifest_alloc_slot_locked(uint32_t *out_slot, uint32_t *out_gen)
{
    if (g_manifest_table.first_free == MANIFEST_SLOT_NIL) {
        uint32_t new_cap = g_manifest_table.capacity * 2u;
        if (new_cap == 0) new_cap = MANIFEST_TABLE_INITIAL_CAP;
        error_t rc = manifest_table_grow_locked(new_cap);
        if (rc != OK) return rc;
    }

    uint32_t s = g_manifest_table.first_free;
    g_manifest_table.first_free = g_manifest_table.slots[s].next_free;
    g_manifest_table.slots[s].next_free = MANIFEST_SLOT_NIL;
    *out_slot = s;
    *out_gen  = g_manifest_table.slots[s].generation;
    g_manifest_table.active_count++;
    return OK;
}

static void manifest_free_slot_locked(uint32_t slot)
{
    g_manifest_table.slots[slot].manifest = NULL;
    g_manifest_table.slots[slot].generation++;  /* invalidates outstanding handles */
    g_manifest_table.slots[slot].next_free = g_manifest_table.first_free;
    g_manifest_table.first_free = slot;
    g_manifest_table.active_count--;
}

/* --------------------------------------------------------------------------
 * CompiledManifest construction / destruction
 * -------------------------------------------------------------------------- */

static void compiled_manifest_destroy(CompiledManifest *cm)
{
    if (!cm) return;
    if (cm->raw_bytes)  kfree(cm->raw_bytes);
    if (cm->handlers)   kfree(cm->handlers);
    if (cm->op_offsets) kfree(cm->op_offsets);
    cm->magic = 0;
    kfree(cm);
}

static error_t manifest_validate_and_index(const uint8_t *bytes,
                                           uint32_t       size,
                                           uint32_t      *out_op_count,
                                           uint32_t     **out_offsets)
{
    if (size < sizeof(Manifest)) return ERR_BUFFER_TOO_SMALL;

    const Manifest *hdr = (const Manifest *)bytes;
    if (hdr->magic != MANIFEST_MAGIC)        return ERR_INVALID_POCKET;
    if (hdr->version != MANIFEST_VERSION)    return ERR_VERSION_MISMATCH;
    if (hdr->total_size != size)             return ERR_INVALID_ARGUMENT;
    if (hdr->op_count == 0)                  return ERR_INVALID_ARGUMENT;

    /* Walk the op stream once to compute offsets and verify bounds. */
    uint32_t *offsets = kmalloc(sizeof(uint32_t) * hdr->op_count);
    if (!offsets) return ERR_NO_MEMORY;

    uint32_t cur = sizeof(Manifest);
    for (uint32_t i = 0; i < hdr->op_count; i++) {
        if (cur + sizeof(ManifestOp) > size) {
            kfree(offsets);
            return ERR_INVALID_ARGUMENT;
        }
        const ManifestOp *op = (const ManifestOp *)(bytes + cur);
        uint32_t op_total = (uint32_t)sizeof(ManifestOp) + (uint32_t)op->param_size;
        if (cur + op_total > size) {
            kfree(offsets);
            return ERR_INVALID_ARGUMENT;
        }
        offsets[i] = cur;
        cur += op_total;
    }
    if (cur != size) {
        kfree(offsets);
        return ERR_INVALID_ARGUMENT;
    }

    *out_op_count = hdr->op_count;
    *out_offsets  = offsets;
    return OK;
}

static error_t manifest_resolve_handlers(const uint8_t           *bytes,
                                         uint32_t                 op_count,
                                         const uint32_t          *offsets,
                                         struct process_t        *owner,
                                         const OpRegistration  ***out_handlers)
{
    const OpRegistration **handlers = kmalloc(sizeof(OpRegistration *) * op_count);
    if (!handlers) return ERR_NO_MEMORY;

    /* Synthesize an OpContext so ManifestOpAuthorize can apply the
     * tag-level policy (OP_AUTH_NONE/APP/UTILITY/SYSTEM/NETWORK) instead of
     * misinterpreting reg->security_mask as a literal bitfield. owner==NULL
     * means kernel-internal (selftests) — ManifestOpAuthorize already
     * fast-paths that to true via the ctx->proc==NULL check. */
    OpContext compile_ctx;
    memset(&compile_ctx, 0, sizeof(compile_ctx));
    compile_ctx.proc = owner;

    for (uint32_t i = 0; i < op_count; i++) {
        const ManifestOp *op = (const ManifestOp *)(bytes + offsets[i]);
        const OpRegistration *reg = OpRegistryLookup(op->op_kind);
        if (!reg) {
            debug_printf("[Manifest] op %u: unregistered op_kind 0x%08x\n", i, op->op_kind);
            kfree(handlers);
            return ERR_INVALID_OPCODE;
        }
        if (!ManifestOpAuthorize(op->op_kind, &compile_ctx)) {
            debug_printf("[Manifest] op %u: auth level %u denied for PID %u\n",
                         i, reg->security_mask,
                         owner ? owner->pid : 0);
            kfree(handlers);
            return ERR_ACCESS_DENIED;
        }
        handlers[i] = reg;
    }
    *out_handlers = handlers;
    return OK;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

error_t ManifestSubsystemInit(void)
{
    if (g_manifest_table.initialized) return ERR_ALREADY_INITIALIZED;

    spinlock_init(&g_manifest_table.lock);
    g_manifest_table.slots        = NULL;
    g_manifest_table.capacity     = 0;
    g_manifest_table.first_free   = MANIFEST_SLOT_NIL;
    g_manifest_table.active_count = 0;

    error_t rc = manifest_table_grow_locked(MANIFEST_TABLE_INITIAL_CAP);
    if (rc != OK) return rc;

    g_manifest_table.initialized = true;
    debug_printf("[Manifest] subsystem ready (initial capacity=%u)\n",
                 MANIFEST_TABLE_INITIAL_CAP);
    return OK;
}

void ManifestSubsystemShutdown(void)
{
    if (!g_manifest_table.initialized) return;

    spin_lock(&g_manifest_table.lock);
    for (uint32_t i = 0; i < g_manifest_table.capacity; i++) {
        if (g_manifest_table.slots[i].manifest) {
            compiled_manifest_destroy(g_manifest_table.slots[i].manifest);
            g_manifest_table.slots[i].manifest = NULL;
        }
    }
    kfree(g_manifest_table.slots);
    g_manifest_table.slots        = NULL;
    g_manifest_table.capacity     = 0;
    g_manifest_table.first_free   = MANIFEST_SLOT_NIL;
    g_manifest_table.active_count = 0;
    g_manifest_table.initialized  = false;
    spin_unlock(&g_manifest_table.lock);
}

error_t ManifestCompile(struct process_t *owner,
                        const void       *user_or_kernel,
                        uint32_t          size,
                        bool              is_kernel_ptr,
                        ManifestHandle   *out_handle)
{
    if (!out_handle) return ERR_NULL_POINTER;
    *out_handle = MANIFEST_HANDLE_INVALID;
    if (!g_manifest_table.initialized) return ERR_NOT_INITIALIZED;
    if (!user_or_kernel || size == 0)  return ERR_INVALID_ARGUMENT;
    if (size < sizeof(Manifest))       return ERR_BUFFER_TOO_SMALL;
    if (size > MANIFEST_RAW_MAX_SIZE) return ERR_INVALID_ARGUMENT;

    /*
     * Step 1: stage the raw Manifest bytes into a kernel-owned buffer.
     *
     *   is_kernel_ptr=true  — user_or_kernel already lives in kernel VA
     *                          (self-tests, boot-time kernel manifests):
     *                          direct memcpy after kmalloc.
     *   is_kernel_ptr=false — user_or_kernel is a userland VA in owner's
     *                          cabin: vmm_user_buf_in walks the user PT
     *                          one phys page at a time, handling non-
     *                          contiguous backing + non-page-aligned
     *                          start/end. The helper does its own kmalloc
     *                          + cleans up on any partial failure, so we
     *                          either get a fully populated buffer or NULL.
     */
    uint8_t *raw = NULL;
    if (is_kernel_ptr) {
        raw = kmalloc(size);
        if (!raw) return ERR_NO_MEMORY;
        memcpy(raw, user_or_kernel, size);
    } else {
        if (!owner || !owner->cabin) return ERR_INVALID_ARGUMENT;
        raw = (uint8_t *)vmm_user_buf_in(owner->cabin->vmm,
                                          (uintptr_t)user_or_kernel,
                                          (size_t)size);
        if (!raw) return ERR_INVALID_ADDRESS;
    }

    /* Step 2: validate and index ops. */
    uint32_t  op_count = 0;
    uint32_t *offsets  = NULL;
    error_t rc = manifest_validate_and_index(raw, size, &op_count, &offsets);
    if (rc != OK) { kfree(raw); return rc; }

    /* Step 3: resolve handlers and run security gate per op. */
    const OpRegistration **handlers = NULL;
    rc = manifest_resolve_handlers(raw, op_count, offsets, owner, &handlers);
    if (rc != OK) { kfree(offsets); kfree(raw); return rc; }

    /* Step 4: build CompiledManifest. */
    CompiledManifest *cm = kmalloc(sizeof(CompiledManifest));
    if (!cm) { kfree(handlers); kfree(offsets); kfree(raw); return ERR_NO_MEMORY; }

    cm->magic       = COMPILED_MANIFEST_MAGIC;
    cm->op_count    = op_count;
    cm->flags       = ((const Manifest *)raw)->flags;
    cm->ref_count   = 1;
    cm->owner_pid   = owner ? owner->pid : 0;
    cm->raw_bytes   = raw;
    cm->raw_size    = size;
    cm->handlers    = handlers;
    cm->op_offsets  = offsets;
    cm->_pad        = 0;
    cm->_pad2       = 0;
    spinlock_init(&cm->lock);

    /* Step 5: allocate slot, install. */
    spin_lock(&g_manifest_table.lock);
    uint32_t slot = 0, gen = 0;
    rc = manifest_alloc_slot_locked(&slot, &gen);
    if (rc != OK) {
        spin_unlock(&g_manifest_table.lock);
        compiled_manifest_destroy(cm);
        return rc;
    }
    cm->handle_slot                       = slot;
    cm->generation                        = gen;
    g_manifest_table.slots[slot].manifest = cm;
    spin_unlock(&g_manifest_table.lock);

    *out_handle = MANIFEST_MAKE_HANDLE(slot, gen);
    debug_printf("[Manifest] compiled handle=0x%lx slot=%u gen=%u op_count=%u\n",
                 (unsigned long)*out_handle, slot, gen, op_count);
    return OK;
}

CompiledManifest *ManifestResolve(ManifestHandle handle)
{
    if (!g_manifest_table.initialized) return NULL;
    uint32_t slot = MANIFEST_HANDLE_SLOT(handle);
    uint32_t gen  = MANIFEST_HANDLE_GEN(handle);

    CompiledManifest *cm = NULL;
    spin_lock(&g_manifest_table.lock);
    if (slot < g_manifest_table.capacity &&
        g_manifest_table.slots[slot].generation == gen &&
        g_manifest_table.slots[slot].manifest) {
        cm = g_manifest_table.slots[slot].manifest;
        __atomic_fetch_add(&cm->ref_count, 1, __ATOMIC_ACQ_REL);
    }
    spin_unlock(&g_manifest_table.lock);
    return cm;
}

error_t ManifestRetain(ManifestHandle handle)
{
    CompiledManifest *cm = ManifestResolve(handle);
    if (!cm) return ERR_INVALID_ARGUMENT;
    /* ManifestResolve already incremented; the caller's prior pin owned the
     * baseline ref. We just keep the extra one we got from Resolve. */
    return OK;
}

error_t ManifestRelease(ManifestHandle handle)
{
    if (!g_manifest_table.initialized) return ERR_NOT_INITIALIZED;
    uint32_t slot = MANIFEST_HANDLE_SLOT(handle);
    uint32_t gen  = MANIFEST_HANDLE_GEN(handle);

    spin_lock(&g_manifest_table.lock);
    if (slot >= g_manifest_table.capacity ||
        g_manifest_table.slots[slot].generation != gen ||
        !g_manifest_table.slots[slot].manifest) {
        spin_unlock(&g_manifest_table.lock);
        return ERR_INVALID_ARGUMENT;
    }
    CompiledManifest *cm = g_manifest_table.slots[slot].manifest;
    uint32_t prev = __atomic_fetch_sub(&cm->ref_count, 1, __ATOMIC_ACQ_REL);
    if (prev == 1) {
        manifest_free_slot_locked(slot);
        spin_unlock(&g_manifest_table.lock);
        compiled_manifest_destroy(cm);
        return OK;
    }
    spin_unlock(&g_manifest_table.lock);
    return OK;
}

uint32_t ManifestActiveCount(void)
{
    if (!g_manifest_table.initialized) return 0;
    return g_manifest_table.active_count;
}

void ManifestReleaseAllForOwner(uint32_t owner_pid)
{
    if (!g_manifest_table.initialized) return;

    /* Stack-bounded batch. A typical cabin holds 0-5 handles; 64 covers
     * pathological cases without growing kernel stack. The outer
     * while-loop catches the case where a cabin held > 64 — we keep
     * draining until a pass finds none. */
    ManifestHandle batch[64];
    uint32_t       collected;
    uint32_t       total_released = 0;

    do {
        collected = 0;
        spin_lock(&g_manifest_table.lock);
        for (uint32_t i = 0;
             i < g_manifest_table.capacity && collected < 64; i++) {
            CompiledManifest *cm = g_manifest_table.slots[i].manifest;
            if (cm && cm->owner_pid == owner_pid) {
                batch[collected++] = MANIFEST_MAKE_HANDLE(
                    i, g_manifest_table.slots[i].generation);
            }
        }
        spin_unlock(&g_manifest_table.lock);

        for (uint32_t i = 0; i < collected; i++) {
            if (ManifestRelease(batch[i]) == OK) total_released++;
        }
    } while (collected == 64);

    if (total_released > 0) {
        debug_printf("[Manifest] auto-released %u handle(s) for dying PID %u\n",
                     total_released, owner_pid);
    }
}

void ManifestDump(ManifestHandle handle)
{
    CompiledManifest *cm = ManifestResolve(handle);
    if (!cm) {
        kprintf("[Manifest] handle 0x%lx invalid\n", (unsigned long)handle);
        return;
    }
    kprintf("[Manifest] handle=0x%lx slot=%u gen=%u op_count=%u flags=0x%x refs=%u\n",
            (unsigned long)handle, cm->handle_slot, cm->generation,
            cm->op_count, cm->flags, __atomic_load_n(&cm->ref_count, __ATOMIC_RELAXED));
    for (uint32_t i = 0; i < cm->op_count; i++) {
        const ManifestOp *op = (const ManifestOp *)(cm->raw_bytes + cm->op_offsets[i]);
        kprintf("  [%u] op_kind=0x%08x deck=%u op=%u in=%u out=%u params=%u → %s\n",
                i, op->op_kind, OP_KIND_DECK(op->op_kind), OP_KIND_OPCODE(op->op_kind),
                op->in_crate, op->out_crate, op->param_size,
                cm->handlers[i]->name ? cm->handlers[i]->name : "(unnamed)");
    }
    ManifestRelease(handle);
}
