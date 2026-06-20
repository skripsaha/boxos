#ifndef STRAND_RINGS_H
#define STRAND_RINGS_H

#include "ktypes.h"

/*
 * strand_rings — per-strand IPC rings + StrandInfo TLS block (P5a).
 *
 * A strand spawned via strand_spawn carves its OWN Pocket/Result/Touch ring
 * headers, ring slot regions, and StrandInfo TLS control block from its
 * Hammock slot (the same per-cabin VA window that already holds the strand's
 * user stack + CET shadow stack — see cabin_layout.h). The kernel routes IPC
 * by process_t.{pocket,result,touch}_ring_phys, so giving each strand its own
 * rings restores single-owner SPSC semantics and removes the shared-ring data
 * race that capped P4 at one concurrent IPC strand per cabin.
 *
 * The main strand has NO carved rings (hammock_base == 0); its rings alias the
 * cabin's fixed-VA rings and its FS base stays 0 / the C++ TCB.
 */

typedef struct process_t process_t;

/*
 * strand_rings_create — build the spawned strand's per-strand rings + TLS.
 *
 * Pre: proc->hammock_base is set (slot reserved under cabin->hammock_lock),
 * proc->cabin / cabin->vmm are valid, and proc->context is initialised
 * (process_init_context_base). The ring slot regions are EAGER-mapped (not
 * lazy): the userspace PocketRing producer writes slot[0] on its first
 * syscall, and the #PF handler only knows the fixed cabin slot VAs, so a lazy
 * Hammock slot would fault fatally.
 *
 * On success: sets proc->{pocket,result,touch}_ring_phys + strandinfo_phys
 * and proc->context.user_fsbase (= StrandInfo VA), then returns true.
 * On failure: unwinds every mapping/allocation and returns false (the four
 * proc fields left 0, user_fsbase unchanged).
 */
bool strand_rings_create(process_t *proc);

/*
 * strand_rings_destroy — release the strand's per-strand rings + TLS.
 *
 * No-op for the main strand (hammock_base == 0). For a spawned strand it
 * clears the ring-header MemTags and unmaps+frees the ring headers, slot
 * regions and StrandInfo. The strand's user stack and CET shadow stack live
 * elsewhere in the slot and are freed by process_free_strand_stack /
 * cet_process_destroy. MUST run while proc->cabin->vmm is still alive (before
 * cabin_ref_dec).
 */
void strand_rings_destroy(process_t *proc);

#endif /* STRAND_RINGS_H */
