#include "../tagfs.h"
#include "use_memory.h"
#include "../settle/settle.h"
#include "tagfs_volume.h"

static inline TagFSLock *UseMemoryLock(TagFSVolume* vol)
{
    return &vol->state.lock;
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
UseSaid UseMemorySay(TagFSVolume* vol, UseText *said, uint64_t saying, uint64_t *ticket)
{
    UseMemoryState *g    = &vol->use_memory;
    UseText        *keep = UseTextHold(said);
    UseText        *gone = NULL;

    TagFSLockTake(UseMemoryLock(vol));
    bool     later  = saying < g->saying;
    UseText *memory = UseTextHold(g->memory);
    bool     quiet  = !g->owed && g->landed;
    uint64_t seen   = g->ticket;
    TagFSLockDrop(UseMemoryLock(vol));

    bool same = !later && quiet && UseTextSame(memory, keep);
    UseTextDrop(memory);

    uint64_t mine = 0;
    TagFSLockTake(UseMemoryLock(vol));
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
    TagFSLockDrop(UseMemoryLock(vol));
    UseTextDrop(gone);
    UseTextDrop(keep);

    if (ticket) *ticket = mine;
    if (later)
        return USE_SAID_SUPERSEDED;
    return same ? USE_SAID_LANDED : USE_SAID_OWED;
}

bool UseMemoryRefused(TagFSVolume* vol, uint64_t ticket)
{
    TagFSLockTake(UseMemoryLock(vol));
    bool refused = ticket && vol->use_memory.refused == ticket;
    TagFSLockDrop(UseMemoryLock(vol));
    return refused;
}

bool UseMemoryKept(TagFSVolume* vol, uint64_t ticket, const UseText *said)
{
    UseMemoryState *g = &vol->use_memory;
    TagFSLockTake(UseMemoryLock(vol));
    bool     answered = g->answered >= ticket && g->landed;
    UseText *now      = UseTextHold(g->memory);
    TagFSLockDrop(UseMemoryLock(vol));
    bool kept = answered && UseTextSame(now, said);
    UseTextDrop(now);
    return kept;
}

error_t tagfs_remember_use_context(TagFSVolume* vol, UseText *said, uint64_t saying, bool *remembered)
{
    uint64_t ticket = 0;
    UseSaid  told   = UseMemorySay(vol, said, saying, &ticket);
    if (told != USE_SAID_OWED) {
        if (remembered) *remembered = told == USE_SAID_LANDED;
        return OK;
    }

    /* A demand on the one writer: a cycle that began after it carries it —
     * or, with no room on the ribbon for it, lays its stitch without it,
     * forgets the word the volume held and refuses it (settle/sewing.c). */
    error_t rc = TagFSSettle(vol);
    if (rc == OK && UseMemoryRefused(vol, ticket))
        rc = ERR_TAGFS_NO_SPACE;
    if (remembered) *remembered = rc == OK && UseMemoryKept(vol, ticket, said);
    return rc;
}

UseText *tagfs_recall_use_context(TagFSVolume* vol, uint64_t *out_saying)
{
    TagFSLockTake(UseMemoryLock(vol));
    UseText *memory = UseTextHold(vol->use_memory.memory);
    uint64_t saying = vol->use_memory.saying;
    TagFSLockDrop(UseMemoryLock(vol));
    if (out_saying) *out_saying = saying;
    return memory;
}

// ----------------------------------------------------------------------------
// The cycle's part: the word whole in the stitch
// ----------------------------------------------------------------------------

bool UseMemoryStitchTake(TagFSVolume* vol, UseText **said, uint64_t *ticket)
{
    UseMemoryState *g = &vol->use_memory;
    *said   = NULL;
    *ticket = 0;
    if (!g->owed)
        return false;
    *said   = UseTextHold(g->said);
    *ticket = g->ticket;
    return true;
}

UseText *UseMemoryStitchRefused(TagFSVolume* vol, uint64_t ticket, bool forgot)
{
    UseMemoryState *g    = &vol->use_memory;
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

UseText *UseMemoryStitchSettled(TagFSVolume* vol, UseText *said, uint64_t ticket)
{
    UseMemoryState *g    = &vol->use_memory;
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

error_t UseMemoryRecall(TagFSVolume* vol, const uint8_t *text, uint64_t len)
{
    UseMemoryState *g    = &vol->use_memory;
    UseText        *said = len ? UseTextMake((const char *)text, (size_t)len) : NULL;
    if (len && !said)
        return ERR_NO_MEMORY;
    TagFSLockTake(UseMemoryLock(vol));
    UseText *gone = g->memory;
    g->memory     = said;
    g->landed     = true;
    TagFSLockDrop(UseMemoryLock(vol));
    UseTextDrop(gone);
    return OK;
}

void UseMemoryLetGo(TagFSVolume* vol)
{
    UseMemoryState *g = &vol->use_memory;
    UseTextDrop(g->said);
    UseTextDrop(g->memory);
    memset(g, 0, sizeof(*g));
}
