#ifndef SYSTEM_DECK_H
#define SYSTEM_DECK_H

#include "ktypes.h"
#include "error.h"
#include "boxos_decks.h"

typedef struct process_t process_t;

/* Manifest opcodes (used by system_ops.c handlers + boxlib wrappers). */
#define SYSTEM_OP_PROC_SPAWN    0x01
#define SYSTEM_OP_PROC_KILL     0x02
#define SYSTEM_OP_PROC_INFO     0x03
#define SYSTEM_OP_CTX_USE       0x04
#define SYSTEM_OP_PROC_EXEC     0x06
#define SYSTEM_OP_INFO          0x07
#define SYSTEM_OP_BUF_ALLOC     0x10
#define SYSTEM_OP_BUF_FREE      0x11
#define SYSTEM_OP_BUF_RESIZE    0x12
#define SYSTEM_OP_DEFRAG_FILE   0x18
#define SYSTEM_OP_FRAG_SCORE    0x19
#define SYSTEM_OP_TAG_ADD       0x20
#define SYSTEM_OP_TAG_REMOVE    0x21
#define SYSTEM_OP_TAG_CHECK     0x22
#define SYSTEM_OP_ROUTE         0x40
#define SYSTEM_OP_ROUTE_TAG     0x41
#define SYSTEM_OP_LISTEN        0x42
#define SYSTEM_OP_PERF_DUMP     0x50

/* Free every buffer owned by the given pid. Called during process teardown.
 * Equivalent to BufferRegistryCleanupProcess(pid) — the wrapper exists for
 * call-site stability across the Phase 12 cleanup. */
void system_deck_cleanup_process_buffers(uint32_t pid);

/* Allocate target->buf_heap pages, copy `length` bytes from sender's heap
 * to target's heap. Returns target user vaddr or 0 on failure. Used by the
 * Manifest-native system.route / system.broadcast ops. */
uint64_t ipc_copy_to_heap(process_t *sender, process_t *target,
                          uint64_t src_addr, uint32_t length);

/* Register Manifest-native System Deck ops. Defined in system_ops.c. */
error_t SystemDeckRegister(void);

#endif /* SYSTEM_DECK_H */
