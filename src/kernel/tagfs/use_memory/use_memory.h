#ifndef TAGFS_USE_MEMORY_H
#define TAGFS_USE_MEMORY_H

#include "../../../lib/kernel/ktypes.h"
#include "../../../include/volume_ledger.h"
#include "../../core/error/error.h"
#include "../../core/use_context/use_context.h"
#include "../plot/plot_width.h"

/*
 * The volume's memory of the Use Context — what its person was doing when
 * they left (use_context.h), kept on the volume in a chain of its own
 * (volume_ledger.h, UseChainLink) that the Ledger names.
 *
 * ‼ THE CHAIN HAS NO CEILING, AND THE LEDGER'S SECTOR CARRIES NO TEXT. The
 * text used to ride in the Ledger's own sector, and a context longer than what
 * the sector held past its fields was forgotten. Now it is as many links as it
 * needs, taken like any other ground, and the Ledger carries only its name.
 *
 * ‼ A CHANGE IS A DEMAND ON THE SETTLE CYCLE, NOT A WRITE. What the person
 * says is handed over (tagfs_remember_use_context) and a cycle carries it: it
 * stakes the links' blocks before its S0, names the chain in the Ledger image
 * it takes at S0, writes the links at S1 — before FLUSH#1, which the new
 * blocks always ask for — and the Ledger naming them at S4. What the medium
 * already holds is never written again, and a volume without ground for the
 * text forgets the old one rather than remembering a word the person no longer
 * says (the owner's rule, 09-08).
 *
 * ‼ A CHAIN GOES BACK ONLY WHEN NO COPY OF THE LEDGER NAMES IT. The two copies
 * are written in turn, so the chain a change replaced stays named by the other
 * copy until the cycle after; and a copy whose write was refused may hold
 * either the chain it named or the one it was given. Each copy's names are kept
 * (the copy names one, or — after a write not proven — two or more), and the
 * replaced chain rests in Fallow until, at the moment it would go back, neither
 * copy names it.
 *
 * Everything here is changed under the volume's state lock and nothing is read
 * off or written to the medium, or asked of the heap, under it.
 */

/* What the Ledger names of a chain (volume_ledger.h). first is
 * VOLUME_LEDGER_NO_BLOCK when it names none. */
typedef struct {
    uint32_t first;
    uint32_t attempt;
    uint64_t seq;
    uint64_t bytes;
    uint32_t crc32;
} UseChainRef;

/* A chain as memory knows it: its name, and its blocks in link order as runs
 * — only the blocks read back as this chain's own links, when it was read. */
typedef struct {
    UseChainRef ref;
    FileExtent *runs;
    uint32_t    run_count;
} UseChain;

/* How a chain read back off the medium. */
typedef enum {
    USE_CHAIN_WHOLE = 0,    /* every link in its place, and the text sums */
    USE_CHAIN_UNHELD,       /* its links check, and this machine could not hold what reading it takes */
    USE_CHAIN_FOREIGN,      /* a link's block holds what it held before: the link's write never landed */
    USE_CHAIN_ROTTEN,       /* its links are its own and do not agree, or the text does not sum */
    USE_CHAIN_UNREAD,       /* the medium would not give a link back */
} UseChainState;

// ----------------------------------------------------------------------------
// The person's word, and the volume's
// ----------------------------------------------------------------------------

/*
 * remember: say `said` (the comma-joined text; NULL says none), the `saying`-th
 * word the machine has said, to the volume. What the medium already holds is
 * not written again; otherwise it is a demand, and a Settle cycle that began
 * after it carries it. A word older than one the volume was already told
 * changes nothing: two sayings that race leave the later one whole.
 * *remembered reports whether the volume now holds exactly this text — false
 * with no ground for it, a cycle refused, or a later word standing. Inside the
 * volume's door with no TagFS lock held.
 *
 * recall: what the volume remembers, in one take — a reference the caller
 * drops (UseTextDrop), or NULL when it remembers none or this machine could not
 * hold it — and in *out_saying (optional) which word it is, 0 for the medium's
 * own.
 */
error_t  tagfs_remember_use_context(UseText *said, uint64_t saying, bool *remembered);
UseText *tagfs_recall_use_context(uint64_t *out_saying);

/* remember's first half, which never waits: the word said to the volume's
 * memory, and what the medium makes of it — a later word stands; the medium
 * holds exactly it already; or it is a demand a cycle that begins after it
 * carries (OWED), whose number goes to *ticket. A program's `use` waits for an
 * OWED one through its Chit (SettleWaitLinkCycle). Inside the door, with no
 * TagFS lock held. */
typedef enum {
    USE_SAID_SUPERSEDED = 0,
    USE_SAID_LANDED,
    USE_SAID_OWED,
} UseSaid;
UseSaid  UseMemorySay(UseText *said, uint64_t saying, uint64_t *ticket);

/* remember's second half, once a cycle that began after demand `ticket` has
 * settled: whether the volume now holds exactly `said` — false when it had no
 * ground for it, or a later word stands. With no TagFS lock held. */
bool     UseMemoryKept(uint64_t ticket, const UseText *said);

// ----------------------------------------------------------------------------
// The Settle cycle's part (settle.h), by the Settle's holder
// ----------------------------------------------------------------------------

/* Before S0, with no TagFS lock held: the demand the cycle carries, and the
 * blocks its chain needs staked (TagFSTakeRun) — none found is the volume
 * forgetting. */
void     UseMemoryStake(void);

/* S0, under the state lock, right after LedgerImageTake: the chain the image
 * names — the new one with the image's seq, or the one the volume holds. */
void     UseMemoryTake(void);

/* S1, with no TagFS lock held: the new chain's links to the medium. OK, or the
 * medium's refusal; and the flush the cycle's FLUSH#1 must be past. */
error_t  UseMemoryWriteLinks(void);
uint64_t UseMemoryLinkNeed(void);

/* Right after S4 put the image on its way, whatever the medium answered: the
 * copy written may now name the image's chain as well as its own. */
void     UseMemoryLedgerWritten(void);

/* S6, under the state lock, before Fallow ripens: the copy written names the
 * image's chain alone, the demand the cycle carried is answered, and a chain
 * the volume stopped remembering rests in Fallow. */
void     UseMemorySettled(void);

/* After the cycle, with no TagFS lock held: `rc` its answer. A refused cycle
 * keeps its new chain for its demand's next cycle; what the cycle let go of is
 * freed, and what the volume forgot is said. */
void     UseMemoryCycleOver(error_t rc);

/* Under the state lock (Fallow's release): whether a copy of the Ledger may
 * name the chain whose first link is `first`. */
bool     UseMemoryNames(uint32_t first);

/* Under the state lock: the blocks the volume's memory holds — the chain it
 * names, and one a refused cycle kept for its demand. */
uint64_t UseMemoryBlocksHeld(void);

// ----------------------------------------------------------------------------
// The mount (with the door shut)
// ----------------------------------------------------------------------------

/*
 * Right after ledger_load: what each copy remembers — the chain it names,
 * read and proven, or a kernel before the chain's text in the record itself —
 * and what the volume remembers from them: the newest copy's; the copy before
 * it when the newest's chain does not read whole (only the Use Context rolls
 * back, never the files); a text carried into a chain by the first cycle; or,
 * when the newest copy is an older kernel's that remembers nothing, a chain
 * found among the blocks nothing names (UseMemoryAfterCensus). A link holding
 * its block's previous content in the chain the newest record's own cycle
 * wrote is the medium's lie about FLUSH#1 (UseMemoryMediumLied). ERR_NO_MEMORY
 * when not even the names fit in memory.
 */
error_t  UseMemoryLoad(void);

/* UseRollCall — GroundOwner GROUND_USE_CHAIN: every link read back of the
 * chains the copies name. Before the census. */
void     UseRollCall(void);

/* Whether the census's orphans are wanted (the newest copy is an older
 * kernel's with no chain), and the census's sink for them. */
bool     UseMemoryWantsOrphans(void);
void     UseMemoryOrphanRun(uint32_t start, uint32_t count, void *ctx);

/* After the census: a whole chain among the orphans taken up, or given to
 * Fallow; the chains no copy's memory stands on given to Fallow. */
void     UseMemoryAfterCensus(void);

/* Whether a link of the chain the newest record's own cycle wrote held its
 * block's previous content (SETTLE_MEDIUM_LIED). */
bool     UseMemoryMediumLied(void);

/* The volume is let go of: everything held of its memory, with not one byte
 * written. */
void     UseMemoryLetGo(void);

// ----------------------------------------------------------------------------
// The medium, read
// ----------------------------------------------------------------------------

/* The chain `ref` names, followed exactly as far as its links say and each one
 * held to the name: into *out its blocks proven its own (UseChainForget lets
 * them go), and into *text (optional) the whole text — NULL unless WHOLE.
 * Inside the door, with no TagFS lock held. */
UseChainState UseChainRead(const UseChainRef *ref, UseText **text, UseChain *out);

/* A chain's runs let go, and the chain named none. */
void     UseChainForget(UseChain *chain);

/* The name a record carries: false when the record carries none of the
 * chain's fields (a kernel before the chain wrote it). */
bool     UseChainRefOf(const VolumeLedger *led, UseChainRef *out);

/*
 * What a mount would remember from two copies of the Ledger (`rec[c]` NULL for
 * a copy that does not read, `inherited[c]` the text a kernel before the chain
 * kept in it): the chain the memory stands on — or the text, and from which
 * copy — the chains no memory stands on, and whether the medium lied. Reads
 * the chains off the medium and changes nothing; UseMemoryVerdictDrop lets it
 * go. The mount's own judge, and the stand's.
 */
typedef enum {
    USE_FROM_NONE = 0,      /* the volume remembers none */
    USE_FROM_NEWEST,        /* the newest copy's chain */
    USE_FROM_OLDER,         /* the copy before it: the newest's chain did not read whole */
    USE_FROM_INHERITED,     /* a kernel before the chain kept the text in the record */
    USE_FROM_ORPHANS,       /* nothing named: the census's orphans are searched */
} UseMemorySource;

typedef struct {
    UseMemorySource from;
    UseChain        chain;          /* NEWEST / OLDER: the chain memory stands on */
    UseText        *text;           /* its text, or the inherited one; NULL when not held */
    UseChainState   newest_state;   /* how the newest copy's chain read */
    UseChainState   older_state;    /* and the other's, when it was read */
    UseChain        loose[VOLUME_LEDGER_COPIES];  /* named by a copy, stood on by no memory */
    uint32_t        loose_count;
    uint32_t        named[VOLUME_LEDGER_COPIES];  /* the first block each copy names */
    uint32_t        attempt;        /* the highest attempt either copy names */
    bool            rolled;         /* the newest copy's chain did not read whole: rolled back */
    bool            lied;
} UseMemoryVerdict;

void     UseMemoryJudge(const VolumeLedger *const rec[VOLUME_LEDGER_COPIES], uint32_t newest,
                        UseText *const inherited[VOLUME_LEDGER_COPIES], UseMemoryVerdict *v);
void     UseMemoryVerdictDrop(UseMemoryVerdict *v);

#endif /* TAGFS_USE_MEMORY_H */
