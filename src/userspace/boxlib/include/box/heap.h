#ifndef BOX_HEAP_H
#define BOX_HEAP_H

#include "box/defs.h"
#include "box/error.h"

// ---------------------------------------------------------------------------
// Standard C allocator interface (thread-safe via umutex)
// ---------------------------------------------------------------------------

void* malloc(size_t size);
void  free(void* ptr);
void* calloc(size_t nmemb, size_t size);
void* realloc(void* ptr, size_t size);

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
