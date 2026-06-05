#ifndef OP_REGISTRY_H
#define OP_REGISTRY_H

#include "ktypes.h"
#include "klib.h"
#include "error.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"

/*
 * OpRegistry — single dispatch table for every operation in BoxOS.
 *
 * Replaces the static deck_table[] in guide.c and the per-deck switch-case
 * dispatch inside *_deck_handler. Decks become passive "vocabulary owners":
 * at init time each deck calls OpRegistryRegister() once per opcode, and the
 * Manifest executor resolves handlers via a single hash lookup.
 *
 * Adding a new driver opcode no longer requires editing a header full of
 * #defines and a switch in the deck handler — drivers register themselves.
 *
 * Concurrency:
 *   - Registration happens only at deck-init time (single core, before SMP up).
 *   - Lookup is read-mostly; a spinlock guards the table for safe rare resize.
 *   - For lock-free reads in steady state, callers may snapshot the table
 *     pointer; resize copies into a new bucket array atomically.
 */

struct process_t;

typedef struct OpContext {
    struct process_t *proc;        /* initiator */
    uint32_t          target_pid;  /* IPC routing target, 0 = result-to-self */
    uint32_t          flags;       /* pocket-level flags propagated from envelope */
    uint16_t          pier_id;     /* urgency lane */
    uint16_t          crate_count; /* mirror of the handler arg, exposed here so
                                    * async handlers can stash it into their
                                    * async_ctx without an extra parameter */
    uint64_t          crates_uaddr;/* user vaddr of the Crate[] array. Async
                                    * handlers that take ownership of the
                                    * staged crates kbuf need this to write
                                    * descriptor mutations back to user memory
                                    * via crate_stage_commit_and_release. */
    bool             *async_owns_crates;
                                   /* Dispatcher-supplied pointer. Async
                                    * handlers set *async_owns_crates = true
                                    * BEFORE returning ERR_WOULD_BLOCK to
                                    * signal that they've stashed the staged
                                    * crates kbuf into their async_ctx and
                                    * will commit+free at I/O completion.
                                    *
                                    * Using an explicit handler-set flag
                                    * (instead of process_get_state == WAITING)
                                    * avoids a race on fast async paths where
                                    * the I/O completes and flips PROC_WORKING
                                    * before the dispatcher rechecks state —
                                    * which would have caused the sync
                                    * cleanup path to double-free the staged
                                    * crates. NULL is treated as false
                                    * (handler refuses, dispatcher cleans). */
} OpContext;

typedef int (*OpHandler)(const ManifestOp *op,
                         Crate            *crates,
                         uint16_t          crate_count,
                         const OpContext  *ctx);

/*
 * security_mask is a bitfield of process-tag requirements.
 * The Manifest compiler ANDs it against process->tag_bits during compile;
 * any bit set in security_mask must also be set in tag_bits. 0 means "any".
 */
typedef struct OpRegistration {
    uint32_t       op_kind;       /* OP_KIND(deck, opcode) — primary key */
    uint32_t       security_mask; /* required tag bits, 0 = no restriction */
    OpHandler      handler;       /* dispatch target */
    const char    *name;          /* "storage.read" for tracing/debug */
} OpRegistration;

/* Lifecycle */
error_t OpRegistryInit(void);
void    OpRegistryShutdown(void);

/* Registration — called by decks at init. Returns ERR_ALREADY_EXISTS if
 * op_kind is already registered (protects against accidental double-register).
 */
error_t OpRegistryRegister(uint32_t    op_kind,
                           OpHandler   handler,
                           uint32_t    security_mask,
                           const char *name);

/* Lookup — returns NULL if not found. Pointer is stable across concurrent
 * readers; only OpRegistryShutdown invalidates it.
 */
const OpRegistration *OpRegistryLookup(uint32_t op_kind);

/* Statistics & introspection */
uint32_t OpRegistryCount(void);
void     OpRegistryDump(void);

#endif /* OP_REGISTRY_H */
