#ifndef BOX_HEAP_H
#define BOX_HEAP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/defs.h"
#include "box/error.h"

// ---------------------------------------------------------------------------
// Tag constants
// ---------------------------------------------------------------------------

#define HEAP_TAG_NONE      0xFF
#define HEAP_TAG_NAME_MAX  32
#define HEAP_TAG_CAP       64

// ---------------------------------------------------------------------------
// Allocation interface (thread-safe via the heap uspin_t — the allocator's
// lock spins by design; see box/sync.h for why it cannot park).
//
// malloc() is a FUNCTION, not a macro. It used to be a variadic macro that
// dispatched malloc(sz) to _malloc_impl and malloc(sz, "tag") to
// malloc_tagged, and that could not survive <cstdlib>: the preprocessor does
// not look at qualification, so `std::malloc(n)` would have expanded to
// `std::_malloc_impl(n)` — a name that does not exist. boxcxx had already
// been routing around the macro by hand (its runtime declares the impl symbol
// itself). The tagged form keeps its own name.
// ---------------------------------------------------------------------------

void* malloc(size_t size);
void* malloc_tagged(size_t size, const char *tag);
void  free(void* ptr);

/* Alignment beyond the 16 bytes malloc already guarantees. The result is
 * released by the ORDINARY free() — the allocator splits a block so that a
 * real header lands in front of the aligned payload, rather than hiding a
 * back-pointer there. `alignment` must be a power of two; anything else, or a
 * zero size, returns NULL. */
void* aligned_alloc(size_t alignment, size_t size);
void* calloc(size_t nmemb, size_t size);
void* realloc(void* ptr, size_t size);

// ---------------------------------------------------------------------------
// Tag registry and query API
// ---------------------------------------------------------------------------

// Register a tag name and return its ID (or existing ID if already registered).
// Returns HEAP_TAG_NONE if the registry is full.
uint8_t heap_register_tag(const char *name);

// Look up a tag by name without creating it.
// Returns HEAP_TAG_NONE if not found.
uint8_t heap_lookup_tag(const char *name);

// Translate a tag ID to its name string.
// Returns NULL if id is HEAP_TAG_NONE or out of range.
const char *heap_tag_name(uint8_t id);

// Count live (non-free) allocations with the given tag name.
size_t heap_count_tag(const char *tag);

// Callback type for iteration.
// Called with heap_lock HELD — must not call malloc/free.
typedef void (*HeapTagCallback)(void *ptr, size_t size, const char *tag_name, void *userdata);

// Iterate all live allocations matching the given tag name.
void heap_iterate_tag(const char *tag, HeapTagCallback cb, void *userdata);

// Iterate all live tagged allocations (tag != HEAP_TAG_NONE).
void heap_iterate_all_tagged(HeapTagCallback cb, void *userdata);

// Print all tagged live blocks via printf.
void heap_dump_tags(void);

// ---------------------------------------------------------------------------
// BoxOS extensions: diagnostics and error reporting
// ---------------------------------------------------------------------------

// Last heap error of the CALLING STRAND (Ф23c). The boxlib heap is shared by
// every strand in the cabin, but "the cause of MY last heap op" is per-strand:
// once a strand has performed a heap operation it reads its OWN cell, so a
// sibling's success can never mask this strand's failure. (Before a spawned
// strand's first heap op, or for a strand that could not claim a pool slot when
// the slab is full, the value comes from the shared main cell — see memory.c.)
// The main strand has its own cell too. Returns:
//   OK                  — no error
//   ERR_CORRUPTED       — block magic mismatch (heap corruption detected)
//   ERR_HEAP_EXHAUSTED  — sbrk failed (out of heap space)
//   ERR_INVALID_ADDRESS — double-free or invalid pointer
//   ERR_NO_MEMORY       — allocation overflow or zero-size
error_t heap_get_last_error(void);

typedef struct {
    size_t total_allocated;     // bytes currently allocated (in-use)
    size_t total_free;          // bytes currently free (in free blocks)
    size_t heap_used;           // total heap bytes consumed from sbrk
    uint32_t alloc_count;       // number of allocated blocks
    uint32_t free_count;        // number of free blocks
    uint32_t malloc_calls;      // lifetime malloc call count
    uint32_t free_calls;        // lifetime free call count
} heap_stats_t;

// Snapshot current heap statistics (thread-safe)
void heap_get_stats(heap_stats_t *out);

// ---------------------------------------------------------------------------
// StrandPool (Ф20e) — per-strand malloc/free magazine cache
// ---------------------------------------------------------------------------

// Flush the calling strand's StrandPool back to the global heap and release its
// slab slot. Called at the top of strand_exit() for a spawned strand and at
// exit() for the main strand, so an orderly death leaves no cached blocks held
// out of the global heap (clean leak diagnostics). Idempotent; a strand that
// never allocated is a no-op.
void strand_pool_flush_self(void);

// Self-test of the crash-orphan reclaim mechanism: stages a spare slab slot as a
// crashed strand would (real cached blocks, slot ORPHANED) and runs the reclaim.
// Returns 1 if every staged block returned to the global heap and the slot is
// FREE again. Exercises the reclaim path deterministically, without a live fault.
int strand_pool_test_orphan_reclaim(unsigned n_blocks);

#ifdef __cplusplus
}
#endif

#endif // BOX_HEAP_H
