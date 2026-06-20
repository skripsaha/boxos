#ifndef BOXCXX_TLS_STRAND_H
#define BOXCXX_TLS_STRAND_H

/*
 * tls_strand.h — per-strand C++ thread_local bootstrap (Ф20b).
 *
 * The MAIN strand's C++ TLS is set up automatically by tls_init.cpp (a
 * priority-101 .init_array ctor). A strand spawned via strand_spawn does NOT
 * run .init_array, and the kernel only sets its fs:0 to the per-strand
 * StrandInfo — the negative-offset .tdata/.tbss image below fs:0 is unpopulated.
 *
 * The strand's entry function MUST call __boxcxx_tls_strand_init() first to
 * copy .tdata + zero .tbss into its own neg-TLS page (eager-mapped by the
 * kernel at HAMMOCK_NEGTLS_PAGE), then __boxcxx_thread_storage_enter() to arm
 * its private thread_local destructor list, and __boxcxx_thread_storage_exit()
 * just before strand_exit() to run those destructors (thread storage duration
 * ends when the strand ends — [basic.start.term]).
 *
 * These are the contract std::thread (Ф20b-2) will wrap; for now they are
 * called by hand from a raw strand_spawn worker.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Populate THIS strand's variant-2 negative-offset C++ TLS block. fs:0 must
 * already be the per-strand StrandInfo (the kernel set it). Mirrors the exact
 * byte layout tls_init.cpp produces for the main strand; does NOT touch fs-base
 * and does NOT run .init_array. Panics if fs:0 is not a StrandInfo or the TLS
 * image does not fit one page. */
void __boxcxx_tls_strand_init(void);

/* Arm this strand's thread_local destructor registry. The head lives in the
 * strand's own neg-TLS (zeroed by the kernel), so this is a no-op today — kept
 * for symmetry with __boxcxx_thread_storage_exit and future per-strand setup. */
void __boxcxx_thread_storage_enter(void);

/* Run + release every thread_local destructor registered on THIS strand, LIFO.
 * Call exactly once, just before the strand terminates (or, for the main
 * strand, before the static destructors at process exit). */
void __boxcxx_thread_storage_exit(void);

#ifdef __cplusplus
}
#endif

#endif /* BOXCXX_TLS_STRAND_H */
