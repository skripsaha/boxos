#include "volume_name.h"
#include "tagfs_volume.h"

static TagFSLock* NameLock(TagFSVolume* vol)
{
    return &vol->state.lock;
}

// ----------------------------------------------------------------------------
// The person's word
// ----------------------------------------------------------------------------

error_t VolumeNameAsk(TagFSVolume* vol, const char* text, size_t len, UseText** said)
{
    *said = NULL;
    if (!TallyNameSpelled((const uint8_t*)text, len))
        return ERR_INVALID_ARGUMENT;
    if (TagFSMediumLeft(vol))
        return ERR_DEVICE_NOT_READY;
    if (TagFSIsUnderGlass(vol) || TagFSReadOnly(vol))
        return ERR_TAGFS_READ_ONLY;
    if (len && !(*said = UseTextMake(text, len)))
        return ERR_NO_MEMORY;
    return OK;
}

bool VolumeNameHolds(TagFSVolume* vol, const UseText* said)
{
    UseText* name = NULL;
    TagFSLockTake(NameLock(vol));
    bool quiet = !vol->volume_name.owed;
    if (quiet)
        name = UseTextHold(vol->volume_name.name);
    TagFSLockDrop(NameLock(vol));
    bool same = quiet && UseTextSame(name, said);
    UseTextDrop(name);
    return same;
}

uint64_t VolumeNameSay(TagFSVolume* vol, UseText* said)
{
    VolumeNameState* g = &vol->volume_name;
    TagFSLockTake(NameLock(vol));
    UseText* gone   = g->said;
    g->said         = said;
    g->owed         = true;
    uint64_t ticket = ++g->ticket;
    TagFSLockDrop(NameLock(vol));
    UseTextDrop(gone);
    return ticket;
}

bool VolumeNameRefused(TagFSVolume* vol, uint64_t ticket)
{
    TagFSLockTake(NameLock(vol));
    bool refused = ticket && vol->volume_name.refused == ticket;
    TagFSLockDrop(NameLock(vol));
    return refused;
}

size_t VolumeNameOf(TagFSVolume* vol, char* out, size_t room)
{
    UseText* name = VolumeNameHeld(vol);
    size_t   len  = name ? name->bytes : 0;
    if (out && len)
        memcpy(out, name->text, len < room ? len : room);
    UseTextDrop(name);
    return len;
}

UseText* VolumeNameHeld(TagFSVolume* vol)
{
    TagFSLockTake(NameLock(vol));
    UseText* name = UseTextHold(vol->volume_name.name);
    TagFSLockDrop(NameLock(vol));
    return name;
}

// ----------------------------------------------------------------------------
// The cycle's part: the name whole in the stitch, under the state lock
// ----------------------------------------------------------------------------

bool VolumeNameStitchTake(TagFSVolume* vol, UseText** said, uint64_t* ticket)
{
    VolumeNameState* g = &vol->volume_name;
    *said   = NULL;
    *ticket = 0;
    if (!g->owed)
        return false;
    *said   = UseTextHold(g->said);
    *ticket = g->ticket;
    return true;
}

/* The name the stitch carried is the book's now, whatever was said since; the
 * demand is met when nothing was. */
UseText* VolumeNameStitchSettled(TagFSVolume* vol, UseText* said, uint64_t ticket)
{
    VolumeNameState* g    = &vol->volume_name;
    UseText*         gone = g->name;
    g->name = said;
    if (g->ticket == ticket)
        g->owed = false;
    return gone;
}

void VolumeNameStitchRefused(TagFSVolume* vol, uint64_t ticket)
{
    VolumeNameState* g = &vol->volume_name;
    if (g->ticket == ticket)
        g->owed = false;
    g->refused = ticket;
}

// ----------------------------------------------------------------------------
// The volume recalled, and let go of
// ----------------------------------------------------------------------------

/* A name the book holds that is not spelled as one — no kernel gives such a
 * name, nor the volume's maker lays one — is not taken up: the volume is
 * shown by its medium, and the next Tally lays no name. */
error_t VolumeNameRecall(TagFSVolume* vol, const uint8_t* text, uint64_t len)
{
    if (!TallyNameSpelled(text, len)) {
        kprintf("[TagFS] volume %u: the name its book holds, %llu byte(s), is not spelled as a name — it is "
                "not taken up, and the volume is shown by its medium\n", TagFSVolumeNumber(vol),
                (unsigned long long)len);
        len = 0;
    }
    UseText* name = len ? UseTextMake((const char*)text, (size_t)len) : NULL;
    if (len && !name)
        return ERR_NO_MEMORY;
    TagFSLockTake(NameLock(vol));
    UseText* gone         = vol->volume_name.name;
    vol->volume_name.name = name;
    TagFSLockDrop(NameLock(vol));
    UseTextDrop(gone);
    return OK;
}

void VolumeNameLetGo(TagFSVolume* vol)
{
    VolumeNameState* g = &vol->volume_name;
    UseTextDrop(g->said);
    UseTextDrop(g->name);
    memset(g, 0, sizeof(*g));
}
