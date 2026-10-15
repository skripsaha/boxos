#ifndef TAGFS_USE_MEMORY_INTERNAL_H
#define TAGFS_USE_MEMORY_INTERNAL_H

#include "../tagfs.h"
#include "use_memory.h"

/*
 * What use_memory.c (the person's word and the Settle cycle), use_memory_mount.c
 * (the mount) and use_chain.c (the medium) share. Not for anyone else.
 */

/* The first blocks one copy of the Ledger may name: one, or more after writes
 * of it that were not proven. Grown with no lock held (UseNamedRoom) and
 * changed under the state lock. */
typedef struct {
    uint32_t *first;
    uint32_t  count;
    uint32_t  cap;
} UseNamed;

/* A run of the census's orphans. */
typedef struct {
    uint64_t start;
    uint32_t count;
} UseOrphanRun;

/*
 * The volume's memory, under the state lock. The Settle's holder alone
 * changes `settled` and `pending`, so it reads them with no lock; the mount
 * and the let-go change everything with the door shut.
 */
typedef struct {
    UseText     *said;       /* the latest word told to the volume; NULL says none */
    uint64_t     saying;     /* which word of the machine's it is; 0 for the medium's own */
    uint64_t     ticket;     /* moves with every demand */
    bool         owed;       /* a cycle still owes `said` to the medium */
    bool         carry;      /* the demand is a kernel-before-the-chain's text carried into one */

    UseChain     settled;    /* the chain the next Ledger names, unless a demand changes it */
    UseText     *memory;     /* its text, or the carried one; NULL with none, or not held */
    bool         landed;     /* the medium's newest copy names `settled` */

    UseChain     pending;    /* written by a refused cycle, for its demand's next one */
    uint64_t     pending_ticket;

    uint64_t     answered;   /* the last demand a cycle answered */
    uint64_t     refused;    /* the last demand a stitch had no room for: refused, not remembered */
    UseNamed     named[VOLUME_LEDGER_COPIES];
    uint32_t     attempt;    /* the last attempt a chain was written as */

    /* The mount's, until UseMemoryAfterCensus. */
    UseChain     loose[VOLUME_LEDGER_COPIES];
    uint32_t     loose_count;
    bool         search;
    bool         carry_known;       /* the newest copy kept a text in the record */
    bool         lied;
    UseOrphanRun *orphans;
    uint32_t     orphan_count;
    uint32_t     orphan_cap;
    bool         orphans_refused;
} UseMemoryState;

extern UseMemoryState g_use_memory;

static inline TagFSLock *UseMemoryLock(void)
{
    return &tagfs_get_state()->lock;
}

/* Whether the memory is exactly what `settled` names: a text and a chain, or
 * neither. Under the state lock. */
static inline bool UseMemoryChained(void)
{
    /* A format-v2 book keeps the text in its Tally and no chain beside it:
     * what memory holds is what the book holds (UseMemoryRecall). */
    if (tagfs_get_state()->format == DEED_FORMAT_V2)
        return true;
    return g_use_memory.memory ? g_use_memory.settled.ref.first != VOLUME_LEDGER_NO_BLOCK
                               : g_use_memory.settled.ref.first == VOLUME_LEDGER_NO_BLOCK;
}

/* Room for one more name in each copy's list, with no lock held. False with
 * no memory for it. */
bool         UseNamedRoom(void);

/* Under the state lock: `first` joins copy `c`'s names; or is its one name. */
void         UseNamedAdd(uint32_t c, uint32_t first);
void         UseNamedOnly(uint32_t c, uint32_t first);

/* A chain rests in Fallow until no copy names it: false, said, with no memory
 * for the node — it then stays taken. With no TagFS lock held. */
bool         UseChainRest(const UseChain *chain, const char *what);

/* The CRC32 of a whole text, of any length. */
uint32_t     UseTextCrc32(const UseText *text);

/* Links per text: ceil(bytes / USE_CHAIN_TEXT). */
uint64_t     UseChainLinksFor(uint64_t bytes);

/* `n` blocks from the allocator, as few runs as it gives them in, near
 * `hint`: OK and *out (runs the caller frees); or its refusal with every block
 * given back, and in *got how many it had found. Inside the door, no lock. */
error_t      UseChainStake(uint64_t n, UseChain *out, uint64_t *got);

/* The blocks of a chain back to the allocator: never named, never written. */
void         UseChainUnstake(UseChain *chain);

/* Every link of `chain`, carrying `text`, to the medium. */
error_t      UseChainWrite(const UseChain *chain, const UseText *text);

/* The census's orphans read for chains: every whole chain among them, newest
 * (seq, attempt) first — into *out (the caller frees each chain's runs and
 * the array) and their texts into *texts, one per chain. */
uint32_t     UseChainFindOrphans(const UseOrphanRun *runs, uint32_t count,
                                 UseChain **out, UseText ***texts);

#endif /* TAGFS_USE_MEMORY_INTERNAL_H */
