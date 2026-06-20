#ifndef BOX_STRAND_H
#define BOX_STRAND_H

#include "box/types.h"
#include "box/error.h"

/*
 * Strands — additional execution contexts inside the caller's cabin.
 *
 * A strand shares the caller's address space (CR3), heap, code and IPC
 * rings, but has its own stack and scheduler state.  This is the kernel
 * substrate for std::thread (wired in a later phase).
 *
 * strand_spawn runs fn(arg) on a fresh strand and returns the new strand's
 * pid (0 on failure).  Synchronise strands with addr_park / addr_wake
 * (box/sync.h) over shared memory — e.g. one strand parks on a flag while
 * another writes it and wakes the waiter.
 *
 * The strand terminates automatically when fn returns (an internal
 * trampoline calls strand_exit); fn may also call strand_exit() itself.
 *
 * malloc/free ARE strand-safe: the boxlib heap serialises every allocation
 * under a single process-wide lock (heap_lock, an atomic test-and-set umutex),
 * so concurrent strands may allocate freely. That single lock does serialise
 * allocations, so a malloc-bound multi-strand workload contends on it; per-
 * strand heap arenas (to remove that contention) are a future scalability
 * step, not a correctness requirement.
 */
uint32_t strand_spawn(void (*fn)(void *arg), void *arg);

/* Terminate the calling strand.  Does not return.  Unlike exit(), it does
 * NOT run global static destructors or flush shared buffers — those belong
 * to the whole cabin and run when the last (main) strand exits. */
void strand_exit(void) __attribute__((noreturn));

#endif /* BOX_STRAND_H */
