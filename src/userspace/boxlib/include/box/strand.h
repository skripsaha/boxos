#ifndef BOX_STRAND_H
#define BOX_STRAND_H

#ifdef __cplusplus
extern "C" {
#endif

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
 * under a single process-wide lock (heap_lock, a test-and-set uspin_t — the one
 * lock that must spin rather than park, since the park path itself allocates),
 * so concurrent strands may allocate freely. That single lock does serialise
 * allocations, so a malloc-bound multi-strand workload contends on it; per-
 * strand heap arenas (to remove that contention) are a future scalability
 * step, not a correctness requirement.
 */
uint32_t strand_spawn(void (*fn)(void *arg), void *arg);

/* Like strand_spawn but JOINABLE: the kernel keeps the finished strand as a
 * zombie (its pid reserved, not reclaimed by the reaper) until strand_release()
 * is called. std::thread uses this so thread::id (== the strand pid) stays
 * unique while the thread is joinable. The owner MUST eventually call
 * strand_release(pid) (join/detach) or the zombie leaks its pid for the cabin's
 * lifetime. Returns the new strand's pid (0 on failure). */
uint32_t strand_spawn_joinable(void (*fn)(void *arg), void *arg);

/* Release a joinable strand (spawned via strand_spawn_joinable): clears its
 * reap-block so the reaper reclaims it. Called by std::thread join()/detach()
 * once its id is no longer needed. Idempotent; a no-op on an unknown pid or a
 * strand of another cabin. */
void strand_release(uint32_t pid);

/* Terminate the calling strand.  Does not return.  Unlike exit(), it does
 * NOT run global static destructors or flush shared buffers — those belong
 * to the whole cabin and run when the last (main) strand exits. */
void strand_exit(void) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* BOX_STRAND_H */
