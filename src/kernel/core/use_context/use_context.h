#ifndef USE_CONTEXT_H
#define USE_CONTEXT_H

#include "ktypes.h"
#include "error.h"
#include "tag_spelling.h"

struct process_t;

/*
 * Use Context — what the person at this machine is doing, said in tags.
 *
 * One per machine, owned by the kernel, set by the user's shell (`use code
 * cpp`) or by a program with system authority. It is not a property of a
 * process or of a cabin: it is the ship's declared condition, and every station
 * reads it and adjusts itself. Two stations do today. The scheduler runs a
 * strand whose cabin wears every context tag in the context tier
 * (SCHED_PRIO_CONTEXT). TagFS narrows a query to the context and stamps a
 * created file with it, unless the caller asked for the whole volume. A later
 * station asks the same question through the same door and needs nothing
 * added here.
 *
 * TAGS, NOT NUMBERS. A tag is a name, `key` or `key:value`. Its number is a
 * page in one particular book, and this machine keeps three books: the mounted
 * volume's registry, the kernel's Logbook, MemTag's registry. So the context
 * holds the NAMES, and saying them never writes a page of any book: `use`
 * only LOOKS its tags up. A tag the volume has no page for yet gets one when a
 * cabin wears it or a file is given it, and that intern tells the context its
 * number (UseContextBindTag). The numbers are bound when the context is set
 * with a volume up, re-bound when a volume is mounted, and dropped when the
 * volume's registry is cleared away.
 *
 * WHAT IS SAID IS ONE PIECE. A context is built once, outside every lock, into
 * an immutable list — the canonical text in the order the person gave it, and
 * beside it the same tags in sorted order for finding one by halves — and it
 * replaces the old one under the context's lock in one store. Every reader
 * holds a reference to one list and reads it with no lock at all, so nobody
 * ever sees half of one context and half of another, and nobody remembers or
 * reports a prefix of what was said.
 *
 * THE SCHEDULER ASKS IN ONE COMPARE. Every change that could change who wears
 * the context — a set, a clear, a binding, any cabin's tags — raises one epoch
 * after it is made. A cabin keeps its verdict as one word, epoch << 1 | match.
 * The scheduler compares the word's epoch with the machine's and takes the
 * match bit either way; a stale one is the previous verdict, and says a
 * recount is owed. The recount runs outside interrupts-off: when the process
 * next enters the kernel, and in a pass on a service loop for cabins that
 * never do.
 *
 * THE VOLUME REMEMBERS IT. What the person said is handed to the volume by
 * reference with every set and clear (UseContextRemember), and a volume coming
 * up under a machine that holds no context is asked what it remembers
 * (UseContextRecall) — so what one was doing is still what one is doing
 * after the night, and it travels with the medium rather than the machine.
 */

/*
 * The context as text, "key,key:value,..." — made once and never changed
 * after, shared by reference between the machine's context and the volume's
 * memory of it. `bytes` does not count the NUL that follows the text.
 */
typedef struct UseText {
    uint32_t refs;
    size_t   bytes;
    char     text[];
} UseText;

/* A text of `bytes` bytes, copied, with one reference; NULL with no memory.
 * Room: the same text uncopied, its bytes the caller's to fill before anyone
 * else holds it. */
UseText *UseTextMake(const char *text, size_t bytes);
UseText *UseTextRoom(size_t bytes);
/* Another reference to `t`, which is returned; NULL stays NULL. */
UseText *UseTextHold(UseText *t);
/* A reference given back; the last one frees. NULL is nothing. */
void     UseTextDrop(UseText *t);
/* Whether two texts say the same bytes; two NULLs do. */
bool     UseTextSame(const UseText *a, const UseText *b);

/*
 * The context as it stands, held: NULL when none is set. The caller reads it
 * with no lock — its tags in the order they were said, each a spelling into
 * the list's own text — and gives it back with UseListDrop.
 */
typedef struct UseList UseList;

UseList       *UseContextHold(void);
void           UseListDrop(UseList *l);
uint32_t       UseListCount(const UseList *l);
TagSpelling    UseListTag(const UseList *l, uint32_t index);
UseText       *UseListText(const UseList *l);

/*
 * Replace the context with the tags of a comma-separated list — "code,cpp" or
 * "code, project:alpha" — of `len` bytes, or up to a NUL before them. A list
 * naming no tag clears. Each tag is read by the one spelling (tag_spelling.h):
 * blanks around it are dropped, "key:" with nothing after the colon is the
 * bare "key", and a tag said twice is kept once, where it was first said. A
 * tag the volume registry could not hold is refused with the spelling's code
 * (ERR_TAGFS_TAG_TOO_LONG, ERR_TAGFS_INVALID_TAG) and the context is left as
 * it was. The volume's numbers are looked up, never issued.
 */
error_t UseContextSet(const char *list, size_t len);
void    UseContextClear(void);

/*
 * The scheduler's question: does this strand's cabin wear EVERY tag of the
 * context? One compare, no lock, nothing of the context read: the cabin's
 * verdict word, previous or current. A stale word is answered with its
 * previous verdict, and the recount it owes is asked for.
 */
bool    UseContextMatches(const struct process_t *proc);

/* The verdict of this strand's cabin made current, counted outside any lock
 * against a snapshot of the context — at the strand's entry into the kernel. */
void    UseContextVerdictRefresh(struct process_t *proc);

/* The recount for every cabin whose verdict is stale, when one is owed: one
 * load when none is. Called from the service loops, with interrupts on. */
void    UseContextServiceIfPending(void);

/* A cabin's tags changed: every verdict is stale from here on. Said after the
 * change is made. */
void    UseContextCabinChanged(void);

/*
 * The volume remembers the context, in a chain of its own the Ledger names
 * (tagfs/use_memory/use_memory.h) — say the current one to it. Done after
 * every set and clear, and when a volume comes up under a context the person
 * had already said. *remembered says whether the volume now holds exactly
 * this context: false with no volume up, no ground on it for the context, a
 * medium that will not take the write, or a later word already said to it.
 * The context is set all the same — the person is told, never refused.
 */
void    UseContextRemember(bool *remembered);

/* The context's text as it stands, and which word it is — for a program's
 * `use`, which tells the volume without waiting inside the kernel
 * (SettleUseRemembered). A reference the caller drops; NULL for none. */
UseText *UseContextSaying(uint64_t *out_saying);

/*
 * A volume is up — said on every road to a mounted volume. A machine holding
 * no context takes up what the volume remembers: the volume hands its memory
 * over in one take and it is read here, outside every lock, and set only if
 * the machine still holds none. A machine that holds one keeps it — the
 * person's word stands over the volume's memory — binds its numbers on the
 * volume that is up now, and tells the volume.
 */
void    UseContextRecall(void);

/*
 * The volume's book is going: that registry is being freed, so its numbers
 * mean nothing from here on. Said with the volume's door drained.
 */
void    UseContextUnbind(void);

/*
 * The mounted volume just gave `tag` the number `tag_id` — a cabin wore it, a
 * file was given it. If the context holds this tag without a number, this is
 * the number. Said inside the volume's door, with no TagFS lock held; one load
 * when the context waits for no number, which is almost always.
 */
void    UseContextBindTag(const TagSpelling *tag, uint32_t tag_id);

void    UseContextInit(void);
void    UseContextShutdown(void);

#ifdef CONFIG_VOLUME_TESTS
/* How many times two tags have been compared inside the context on this core
 * — finding a tag in the list, finding a number in the snapshot — since boot.
 * The stand counts it across the scheduler's question to see that the
 * question compares none. */
uint64_t UseContextCompares(void);
#endif

#endif /* USE_CONTEXT_H */
