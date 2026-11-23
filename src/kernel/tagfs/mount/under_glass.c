#include "../tagfs.h"
#include "crypto.h"
#include "amp.h"
#include "tagfs_volume.h"

/*
 * UnderGlass (tagfs.h): the volume's book carries what this kernel's writer
 * could not carry on, so the volume is read and nothing of it is written.
 *
 * The verdict is the mount's: it is given while the book is read, with the
 * door shut and one core at it (tagfs.c, the attending core), and taken away
 * only when the volume is let go of — never by the reset of Ink a mount ends
 * with (InkMountReset), nor by a medium seated again (InkReadOnlyLift), which
 * are about the medium, not the book. So the reasons need no lock: one core
 * writes them, before any other can come in.
 *
 * Each reason is said once a mount: a book whose every one of a hundred tags
 * is RETIRED says so a hundred times, but the same sentence twice is one
 * reason. They are known by their sum, the first GLASS_SAID_MOST of them;
 * past that each is said as it comes.
 */


void TagFSUnderGlass(TagFSVolume* vol, const char* why)
{
    if (!why) {
        __atomic_store_n(&vol->under_glass.glass, 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&vol->under_glass.door_said, 0u, __ATOMIC_RELEASE);
        vol->under_glass.said_count = 0;
        return;
    }
    __atomic_store_n(&vol->under_glass.glass, 1u, __ATOMIC_RELEASE);
    uint32_t sum = KCrc32c((const uint8_t*)why, (uint32_t)strlen(why));
    for (uint32_t i = 0; i < vol->under_glass.said_count; i++)
        if (vol->under_glass.said[i] == sum)
            return;
    if (vol->under_glass.said_count < GLASS_SAID_MOST)
        vol->under_glass.said[vol->under_glass.said_count++] = sum;
    kprintf("[TagFS] the volume may only be read: %s\n", why);
}

bool TagFSIsUnderGlass(TagFSVolume* vol)
{
    return __atomic_load_n(&vol->under_glass.glass, __ATOMIC_ACQUIRE) != 0;
}

/*
 * ‼ A DOOR THAT WRITES, REACHED UNDER GLASS, IS A DEFECT ABOVE IT. Every road
 * a change takes is refused where it begins — Ink, the Settle cycle, the halt
 * (TagFSReadOnly, SettleAsked) — so a write that arrives at the medium's own
 * door on such a volume came by a road nobody shut. It is refused there all
 * the same, before a byte, and named: a STRICT kernel stops at it.
 */
void TagFSGlassHaltSaid(uint8_t seat)
{
    kprintf("[HALT] the volume on seat %u stands under glass: this kernel wrote nothing to it, "
            "and there is nothing to make durable\n", seat);
}

#ifdef CONFIG_VOLUME_TESTS
static volatile uint32_t s_knocker;      /* the core the stand knocks from, + 1; 0: nobody */
static volatile uint32_t s_knocks;

void TagFSGlassKnocking(bool on)
{
    __atomic_store_n(&s_knocker, on ? amp_get_core_index() + 1u : 0u, __ATOMIC_RELEASE);
}

uint32_t TagFSGlassKnocks(void)
{
    return __atomic_load_n(&s_knocks, __ATOMIC_ACQUIRE);
}

/* Whether this refusal is the stand's own knock, counted and said as one. */
static bool GlassKnocked(const char* door)
{
    if (__atomic_load_n(&s_knocker, __ATOMIC_ACQUIRE) != amp_get_core_index() + 1u)
        return false;
    __atomic_add_fetch(&s_knocks, 1u, __ATOMIC_ACQ_REL);
    kprintf("[TESTS] %s, knocked at by the stand under glass, refused before a byte\n", door);
    return true;
}
#endif

error_t TagFSGlassRefused(TagFSVolume* vol, const char* door)
{
#ifdef CONFIG_VOLUME_TESTS
    if (GlassKnocked(door))
        return ERR_TAGFS_READ_ONLY;
#endif
#if CONFIG_STRICT
    panic("[TagFS] DEFECT: %s was asked to write a volume that stands under glass", door);
#endif
    if (!__atomic_exchange_n(&vol->under_glass.door_said, 1u, __ATOMIC_ACQ_REL))
        kprintf("[TagFS] DEFECT: %s was asked to write a volume that stands under glass — "
                "refused before a byte (ERR_TAGFS_READ_ONLY)\n", door);
    return ERR_TAGFS_READ_ONLY;
}
