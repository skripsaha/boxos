#ifndef BOXCXX_TLS_STRAND_H
#define BOXCXX_TLS_STRAND_H

/*
 * tls_strand.h — the thread_local destructors of a strand (Ф20b).
 *
 * The kernel makes every strand's thread-local storage from the template the
 * program carries before the strand runs its first instruction — the main
 * strand's and each spawned one's alike — so a thread_local reads its initial
 * value without anything run first. What is left to the strand is the end of
 * thread storage duration: a strand entry calls __boxcxx_thread_storage_enter()
 * before its work and __boxcxx_thread_storage_exit() just before strand_exit(),
 * which runs that strand's thread_local destructors ([basic.start.term]).
 *
 * These are the contract std::thread wraps; a raw strand_spawn worker calls
 * them by hand.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Arm this strand's thread_local destructor registry. The head lives in the
 * strand's own thread-local storage, which starts zeroed, so this is a no-op
 * today — kept for symmetry with __boxcxx_thread_storage_exit and future
 * per-strand setup. */
void __boxcxx_thread_storage_enter(void);

/* Run + release every thread_local destructor registered on THIS strand, LIFO.
 * Call exactly once, just before the strand terminates (or, for the main
 * strand, before the static destructors at process exit). */
void __boxcxx_thread_storage_exit(void);

#ifdef __cplusplus
}
#endif

#endif /* BOXCXX_TLS_STRAND_H */
