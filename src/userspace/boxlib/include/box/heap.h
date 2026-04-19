#ifndef BOX_HEAP_H
#define BOX_HEAP_H

#include "box/defs.h"
#include "box/error.h"

// ---------------------------------------------------------------------------
// Tag constants
// ---------------------------------------------------------------------------

#define HEAP_TAG_NONE      0xFF
#define HEAP_TAG_NAME_MAX  32
#define HEAP_TAG_CAP       64

// ---------------------------------------------------------------------------
// Internal allocation functions — call via malloc() macro below
// ---------------------------------------------------------------------------

void* _malloc_impl(size_t size);
void* malloc_tagged(size_t size, const char *tag);

// ---------------------------------------------------------------------------
// Variadic malloc dispatch:
//   malloc(size)        -> _malloc_impl(size)
//   malloc(size, "tag") -> malloc_tagged(size, "tag")
// ---------------------------------------------------------------------------

#define _MALLOC_NARG(...)              _MALLOC_NARG_I(__VA_ARGS__, 2, 1)
#define _MALLOC_NARG_I(_1, _2, N, ...) N
#define _MALLOC_CAT(a, b)              _MALLOC_CAT_I(a, b)
#define _MALLOC_CAT_I(a, b)            a##b
#define _malloc_1(sz)                  _malloc_impl(sz)
#define _malloc_2(sz, tag)             malloc_tagged((sz), (tag))
#define malloc(...)  _MALLOC_CAT(_malloc_, _MALLOC_NARG(__VA_ARGS__))(__VA_ARGS__)

// ---------------------------------------------------------------------------
// Standard allocator interface (thread-safe via umutex)
// ---------------------------------------------------------------------------

void  free(void* ptr);
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

// Last heap error (per-process, set on failure)
//   OK                  — no error
//   ERR_CORRUPTED       — block magic mismatch (heap corruption detected)
//   ERR_HEAP_EXHAUSTED  — sbrk failed (out of heap space)
//   ERR_INVALID_ADDRESS — double-free or invalid pointer
//   ERR_NO_MEMORY       — allocation overflow or zero-size
extern error_t heap_last_error;

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

#endif // BOX_HEAP_H
