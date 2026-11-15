#ifndef SYSTEM_DECK_H
#define SYSTEM_DECK_H

#include "ktypes.h"
#include "error.h"
#include "boxos_decks.h"
#include "boxos_system_info.h"   /* SystemInfo, SystemResources, EfiInfo — the answers, shared with boxlib */

typedef struct process_t process_t;
typedef struct Question Question;

/* All SYSTEM_OP_* opcodes live in src/include/boxos_decks.h (included
 * above) — single source of truth for kernel handler tables and boxlib
 * syscall wrappers. NEVER add a SYSTEM_OP_* define here. */

/* The deposit window (system_deck.c, cabin.h deposit_*): what the kernel
 * lends a program to read, in whole pages of its own. */

/* Places for `length` bytes in the target's window, mapped and the kernel's
 * to write; 0 when there is no room. cabin_heap_lend settles them. */
uint64_t cabin_heap_place(process_t *target, uint32_t length);

/* The places cabin_heap_place gave, written: lent to the program, which may
 * give them back from now on — or, when the writing failed, let go at once. */
void cabin_heap_lend(process_t *target, uint64_t addr, uint32_t length, bool written);

/* A deposit given back: unmapped, its frames back to the machine, its places
 * free for the next. ERR_INVALID_ADDRESS, and nothing taken, unless every
 * page from addr for length bytes is lent to the program now. */
error_t cabin_heap_take_back(process_t *target, uint64_t addr, uint32_t length);

/* `length` bytes of a KERNEL buffer lent to the target: its user vaddr, or 0
 * on failure. What system.proc.exec uses to hand a cabin Luggage too long for
 * its CabinInfo page, and system.ask a question. */
uint64_t cabin_heap_deposit(process_t *target, const void *kbuf, uint32_t length);

/* The same from the sender's memory: what system.route and system.broadcast
 * lend each receiver. */
uint64_t ipc_copy_to_heap(process_t *sender, process_t *target,
                          uint64_t src_addr, uint32_t length);

/* Settle one question (system_ops.c, system.ask): the answer into the asker's
 * answer box, or rc in its place, and the asker answered. Touches the asker's
 * memory only to put an answer there — a refusal and a death touch none of
 * it, so a death settles from wherever the life ended (ProcessGoneDeliver).
 * Frees the question; returns what the asker was answered. */
error_t QuestionSettle(Question *q, error_t rc, const void *answer, uint32_t length);

/* Register Manifest-native System Deck ops. Defined in system_ops.c. */
error_t SystemDeckRegister(void);

/* Boot self-test for the proc-mutation authority predicate (kill/tag gate).
 * Defined in system_ops.c next to the static proc_has_authority_over it drives. */
error_t ProcAuthSelfTest(void);

/* Register Touch ops. Defined in touch_ops.c. Called from SystemDeckRegister. */
error_t TouchOpsRegister(void);

/* Register Bay ops + heap-prefault. Defined in bay_ops.c. Called from
 * SystemDeckRegister alongside TouchOpsRegister. */
error_t BayOpsRegister(void);

/* Register Brook ops. Defined in brook_ops.c. Called from
 * SystemDeckRegister alongside Bay/Touch registration. */
error_t BrookOpsRegister(void);

/* MemTag — RAM-region tagging, TagFS-shaped.
 * Opcodes (SYSTEM_OP_MEMTAG_QUERY..APPLY_PKEY, 0xA0–0xAA) live in the
 * shared header `src/include/boxos_decks.h` so kernel + boxlib see the
 * same numeric values via a single source of truth. */

error_t MemTagOpsRegister(void);

/* HW per-process real-HW state ops (LAM, future CET/TME context).
 * Defined in hw_ops.c. */
error_t HwOpsRegister(void);

/* Strand sync: addr_park / addr_wake. Defined in sync_ops.c. */
error_t SyncOpsRegister(void);

/* Register SYSTEM_OP_TURN_IN (turnin_ops.c) — the sleep a strand takes when
 * nothing it waits on has arrived yet. Called from SystemDeckRegister. */
error_t TurnInOpsRegister(void);

/* Register the Clipboard ops (clipboard_ops.c) — the machine's list of what
 * was copied. Called from SystemDeckRegister. */
error_t ClipboardOpsRegister(void);

/* Register SYSTEM_OP_ROSTER (roster_ops.c) — every program aboard, each with
 * its strands. Called from SystemDeckRegister. */
error_t RosterOpsRegister(void);

/* Register SYSTEM_OP_JOURNEY (journey.c) — the loader's moments and the
 * kernel's, from power-on to a shell. Called from SystemDeckRegister. */
error_t JourneyOpsRegister(void);

#endif /* SYSTEM_DECK_H */
