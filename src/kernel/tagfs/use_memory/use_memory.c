#include "../tagfs.h"
#include "use_memory.h"
#include "../settle/settle.h"

/*
 * The volume's memory of the Use Context, as the machine changes it (use_memory.h):
 * the person's word handed over, and the stitch's part in carrying it.
 *
 * Under the state lock; the mount and the let-go change everything with the
 * door shut.
 */
typedef struct {
    UseText *said;       /* the latest word told to the volume; NULL says none */
    uint64_t saying;     /* which word of the machine's it is; 0 for the medium's own */
    uint64_t ticket;     /* moves with every demand */
    bool     owed;       /* a cycle still owes `said` to the medium */
    UseText *memory;     /* what the book holds; NULL with none, or not held */
    bool     landed;     /* the book's word is known: recalled at the mount, or settled since */
    uint64_t answered;   /* the last demand a cycle answered */
    uint64_t refused;    /* the last demand a stitch had no room for: refused, not remembered */
} UseMemoryState;

static UseMemoryState g_use_memory;

static inline TagFSLock *UseMemoryLock(void)
{
    return &tagfs_get_state()->lock;
}

// ----------------------------------------------------------------------------
// The person's word
// ----------------------------------------------------------------------------

/*
 * ‼ WHAT THE MEDIUM ALREADY HOLDS IS NOT WRITTEN AGAIN. A replug recalls the
 * context and remembers it at once; a memory the newest copy names, with no
 * demand on its way, answers at once and costs nothing — the texts are
 * compared outside the lock, and a demand made while they were is seen by
 * the second look.
 */
UseSaid UseMemorySay(UseText *said, uint64_t saying, uint64_t *ticket)
{
    UseMemoryState *g    = &g_use_memory;
    UseText        *keep = UseTextHold(said);
    UseText        *gone = NULL;

    TagFSLockTake(UseMemoryLock());
    bool     later  = saying < g->saying;
    UseText *memory = UseTextHold(g->memory);
    bool     quiet  = !g->owed && g->landed;
    uint64_t seen   = g->ticket;
    TagFSLockDrop(UseMemoryLock());

    bool same = !later && quiet && UseTextSame(memory, keep);
    UseTextDrop(memory);

    uint64_t mine = 0;
    TagFSLockTake(UseMemoryLock());
    later = later || saying < g->saying;
    if (!later) {
        same = same && g->ticket == seen && !g->owed;
        gone        = g->said;
        g->said     = keep;
        keep        = NULL;
        g->saying   = saying;
        if (!same) {
            g->owed = true;
            mine    = ++g->ticket;
        }
    }
    TagFSLockDrop(UseMemoryLock());
    UseTextDrop(gone);
    UseTextDrop(keep);

    if (ticket) *ticket = mine;
    if (later)
        return USE_SAID_SUPERSEDED;
    return same ? USE_SAID_LANDED : USE_SAID_OWED;
}

bool UseMemoryRefused(uint64_t ticket)
{
    TagFSLockTake(UseMemoryLock());
    bool refused = ticket && g_use_memory.refused == ticket;
    TagFSLockDrop(UseMemoryLock());
    return refused;
}

bool UseMemoryKept(uint64_t ticket, const UseText *said)
{
    UseMemoryState *g = &g_use_memory;
    TagFSLockTake(UseMemoryLock());
    bool     answered = g->answered >= ticket && g->landed;
    UseText *now      = UseTextHold(g->memory);
    TagFSLockDrop(UseMemoryLock());
    bool kept = answered && UseTextSame(now, said);
    UseTextDrop(now);
    return kept;
}

error_t tagfs_remember_use_context(UseText *said, uint64_t saying, bool *remembered)
{
    uint64_t ticket = 0;
    UseSaid  told   = UseMemorySay(said, saying, &ticket);
    if (told != USE_SAID_OWED) {
        if (remembered) *remembered = told == USE_SAID_LANDED;
        return OK;
    }

    /* A demand on the one writer: a cycle that began after it carries it —
     * or, with no room on the ribbon for it, lays its stitch without it,
     * forgets the word the volume held and refuses it (settle/sewing.c). */
    error_t rc = TagFSSettle();
    if (rc == OK && UseMemoryRefused(ticket))
        rc = ERR_TAGFS_NO_SPACE;
    if (remembered) *remembered = rc == OK && UseMemoryKept(ticket, said);
    return rc;
}

UseText *tagfs_recall_use_context(uint64_t *out_saying)
{
    TagFSLockTake(UseMemoryLock());
    UseText *memory = UseTextHold(g_use_memory.memory);
    uint64_t saying = g_use_memory.saying;
    TagFSLockDrop(UseMemoryLock());
    if (out_saying) *out_saying = saying;
    return memory;
}

// ----------------------------------------------------------------------------
// The cycle's part: the word whole in the stitch
// ----------------------------------------------------------------------------

bool UseMemoryStitchTake(UseText **said, uint64_t *ticket)
{
    UseMemoryState *g = &g_use_memory;
    *said   = NULL;
    *ticket = 0;
    if (!g->owed)
        return false;
    *said   = UseTextHold(g->said);
    *ticket = g->ticket;
    return true;
}

UseText *UseMemoryStitchRefused(uint64_t ticket, bool forgot)
{
    UseMemoryState *g    = &g_use_memory;
    UseText        *gone = NULL;
    if (forgot) {
        gone      = g->memory;
        g->memory = NULL;
        g->landed = true;
    }
    if (g->ticket == ticket)
        g->owed = false;
    if (ticket > g->answered)
        g->answered = ticket;
    g->refused = ticket;
    return gone;
}

UseText *UseMemoryStitchSettled(UseText *said, uint64_t ticket)
{
    UseMemoryState *g    = &g_use_memory;
    UseText        *gone = g->memory;
    g->memory = said;
    g->landed = true;
    if (g->ticket == ticket)
        g->owed = false;
    if (ticket > g->answered)
        g->answered = ticket;
    return gone;
}

// ----------------------------------------------------------------------------
// The volume recalled, and let go of
// ----------------------------------------------------------------------------

error_t UseMemoryRecall(const uint8_t *text, uint64_t len)
{
    UseMemoryState *g    = &g_use_memory;
    UseText        *said = len ? UseTextMake((const char *)text, (size_t)len) : NULL;
    if (len && !said)
        return ERR_NO_MEMORY;
    TagFSLockTake(UseMemoryLock());
    UseText *gone = g->memory;
    g->memory     = said;
    g->landed     = true;
    TagFSLockDrop(UseMemoryLock());
    UseTextDrop(gone);
    return OK;
}

void UseMemoryLetGo(void)
{
    UseMemoryState *g = &g_use_memory;
    UseTextDrop(g->said);
    UseTextDrop(g->memory);
    memset(g, 0, sizeof(*g));
}
