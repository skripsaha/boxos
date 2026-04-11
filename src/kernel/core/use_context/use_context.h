#ifndef USE_CONTEXT_H
#define USE_CONTEXT_H

#include "ktypes.h"
#include "process.h"
#include "error.h"

// ============================================================================
// Use Context — Global Tag-Based Process Focus
// ============================================================================
// When enabled, processes matching ALL context tags receive priority boost.
// Uses AND semantics: process must have every tag in the context.

typedef struct {
    uint64_t    context_bits;       // Tags 0-63 as bitmask
    uint16_t   *overflow_ids;       // Tags >= 64
    uint16_t    overflow_count;
    uint16_t    overflow_capacity;
    bool        enabled;
} UseContext;

// ============================================================================
// Public API
// ============================================================================

// Initialize use-context subsystem
void UseContextInit(void);

// Shutdown and free resources
void UseContextShutdown(void);

// Set use context from tag strings (e.g., ["app", "utility"])
// count = 0 clears the context
error_t UseContextSet(const char *tags[], uint32_t count);

// Clear use context
void UseContextClear(void);

// Check if process matches current use context
bool UseContextMatches(const process_t *proc);

// Get current context state (read-only)
const UseContext *UseContextGet(void);

#endif // USE_CONTEXT_H
