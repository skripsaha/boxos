#include "integrity_internal.h"
#include "../mount/mount.h"
#include "../../lib/kernel/klib.h"
#include "touch.h"

/*
 * The rotten blocks this mount has named.
 *
 * A block whose bytes do not match the digest recorded when it was written is
 * named the moment a read finds it: one line that says which block and why it
 * may be, and one Touch on the "integrity" tag — each once, however often the
 * block is read after, until it is written again or given back: what it holds
 * then has a digest of its own, and rot in it is new rot. What remembers that
 * it was named is a set that grows as rot is found — the machine is told of
 * every rotten block, not the first 64 a ring had room for (the 65th was
 * counted and never told). A set that cannot grow, the heap spent, still has
 * each rot published and answered to its reader; only its own line is not
 * said, and how many went unnamed is said when the volume is let go: "and N
 * more".
 *
 * The set is open-addressed on block + 1 (0 is an empty slot), at most half
 * full, grown with its lock let go: the bigger table is allocated outside it
 * and swapped in under it. Its own leaf lock, never the map's: a rot is named
 * from inside a read, and naming it holds no page of the map.
 */
static TagFSLock g_rot_lock  = TAGFS_LOCK_INIT("rot", LOCK_RANK_LEAF);
static uint64_t* g_rot_set;
static uint32_t  g_rot_slots;      /* a power of two, or 0 */
static uint32_t  g_rot_named;      /* blocks in the set */
static uint32_t  g_rot_more;       /* rot found that the set had no room to name */
static bool      g_rot_room_said;  /* the set's want of room said for this run of misses */
static uint32_t  g_rot_tag = TAGFS_INVALID_TAG_ID;

typedef enum {
    ROT_NEW,      /* named now, for the first time this mount */
    ROT_KNOWN,    /* named before */
    ROT_NO_ROOM   /* the set could not grow to hold it */
} RotNaming;

static uint32_t RotSlotOf(uint64_t block, uint32_t slots)
{
    return (uint32_t)((block * 2654435761u) & (slots - 1u));
}

/* Under g_rot_lock: the set holds `block`, or it is put in when there is room. */
static bool RotSetFind(uint64_t block, bool put)
{
    if (g_rot_slots == 0)
        return false;
    uint64_t key = block + 1u;
    for (uint32_t i = RotSlotOf(block, g_rot_slots); ; i = (i + 1u) & (g_rot_slots - 1u)) {
        if (g_rot_set[i] == key)
            return true;
        if (g_rot_set[i] == 0) {
            if (put) {
                g_rot_set[i] = key;
                g_rot_named++;
            }
            return false;
        }
    }
}

/* Under g_rot_lock: `block` out of the set, the entries after it in its probe
 * run shifted back over the hole, so every one is still found from its home. */
static void RotSetDrop(uint64_t block)
{
    if (g_rot_slots == 0)
        return;
    uint32_t mask = g_rot_slots - 1u;
    uint64_t key  = block + 1u;
    uint32_t hole = RotSlotOf(block, g_rot_slots);
    while (g_rot_set[hole] != key) {
        if (g_rot_set[hole] == 0)
            return;
        hole = (hole + 1u) & mask;
    }
    for (uint32_t j = (hole + 1u) & mask; g_rot_set[j] != 0; j = (j + 1u) & mask) {
        uint32_t home = RotSlotOf(g_rot_set[j] - 1u, g_rot_slots);
        /* The entry at j may fill the hole unless its home lies after the
         * hole and at or before j, cyclically. */
        bool stays = hole <= j ? (home > hole && home <= j) : (home > hole || home <= j);
        if (!stays) {
            g_rot_set[hole] = g_rot_set[j];
            hole            = j;
        }
    }
    g_rot_set[hole] = 0;
    g_rot_named--;
}

/* The set's slots once more as many, the old table handed back to be freed
 * with the lock let go. Under g_rot_lock. */
static uint64_t* RotSetTakeBigger(uint64_t* bigger, uint32_t slots)
{
    uint64_t* old       = g_rot_set;
    uint32_t  old_slots = g_rot_slots;
    g_rot_set   = bigger;
    g_rot_slots = slots;
    g_rot_named = 0;
    for (uint32_t i = 0; i < old_slots; i++) {
        if (old[i])
            (void)RotSetFind(old[i] - 1u, true);
    }
    return old;
}

static RotNaming RotRemember(uint64_t block)
{
    for (;;) {
        TagFSLockTake(&g_rot_lock);
        if (RotSetFind(block, false)) {
            TagFSLockDrop(&g_rot_lock);
            return ROT_KNOWN;
        }
        if ((g_rot_named + 1u) * 2u <= g_rot_slots) {
            (void)RotSetFind(block, true);
            g_rot_room_said = false;
            TagFSLockDrop(&g_rot_lock);
            return ROT_NEW;
        }
        uint32_t slots = g_rot_slots;
        TagFSLockDrop(&g_rot_lock);

        uint32_t  want   = slots ? slots * 2u : 64u;
        uint64_t* bigger = want > slots ? kmalloc((size_t)want * sizeof(uint64_t)) : NULL;
        if (!bigger) {
            TagFSLockTake(&g_rot_lock);
            g_rot_more++;
            bool said       = g_rot_room_said;
            g_rot_room_said = true;
            TagFSLockDrop(&g_rot_lock);
            if (!said)
                kprintf("[Integrity] the set of rotten blocks named this mount holds %u and "
                        "has no memory to grow: rot found from here on is still published on "
                        "the integrity tag and answered to its reader, counted, and said when "
                        "the volume is let go\n", slots / 2u);
            return ROT_NO_ROOM;
        }
        memset(bigger, 0, (size_t)want * sizeof(uint64_t));

        uint64_t* old = NULL;
        TagFSLockTake(&g_rot_lock);
        if (g_rot_slots == slots)
            old = RotSetTakeBigger(bigger, want);
        else
            old = bigger;          /* grown by another meanwhile: this one is not needed */
        TagFSLockDrop(&g_rot_lock);
        kfree(old);
    }
}

void IntegrityRotBegin(uint32_t tag)
{
    TagFSLockTake(&g_rot_lock);
    uint64_t* old   = g_rot_set;
    g_rot_set       = NULL;
    g_rot_slots     = 0;
    g_rot_named     = 0;
    g_rot_more      = 0;
    g_rot_room_said = false;
    g_rot_tag       = tag;
    TagFSLockDrop(&g_rot_lock);
    kfree(old);
}

/* A block whose read is refused, published on the integrity tag. The event
 * carries the u32 block its readers are built for (box/touch.h — the
 * programs' side widens with wave 3): a block past one is never published cut
 * to another block's number; the line names it whole. */
static void RotPublish(uint64_t block)
{
    uint32_t tag = __atomic_load_n(&g_rot_tag, __ATOMIC_RELAXED);
    if (tag != TAGFS_INVALID_TAG_ID && block <= UINT32_MAX) {
        struct __attribute__((packed)) {
            uint32_t block;
            uint8_t  op;        /* 3: a block that does not hold what was written to it */
            uint8_t  _pad[3];
        } ev = { (uint32_t)block, 3, { 0, 0, 0 } };
        TouchPublishId(tag, &ev, sizeof(ev), 0, TOUCH_FLAG_TAGFS);
    }
}

void IntegrityRotName(uint64_t block, uint64_t stored, uint64_t read)
{
    RotNaming naming = RotRemember(block);
    if (naming == ROT_KNOWN)
        return;
    RotPublish(block);
    if (naming != ROT_NEW)
        return;

    /* Format v2 writes no block in place (spec_v2 §8.1): a closed block that
     * does not match is rot, whatever the last stop was. */
    uint64_t cycle = 0;
    if (tagfs_get_state()->format == DEED_FORMAT_V2 || MountStopWasClean(&cycle))
        kprintf("[Integrity] block %llu does not hold what was written to it (digest %016llx, "
                "read %016llx) — rot; a read of it is refused ERR_CHECKSUM, and it is "
                "published on the integrity tag\n", (unsigned long long)block,
                (unsigned long long)stored, (unsigned long long)read);
    else
        kprintf("[Integrity] block %llu does not match its digest (digest %016llx, read %016llx), "
                "on a volume whose last stop is not known to have been clean (seal of cycle "
                "%llu) — rot, or a write in place before that stop that did not land: format "
                "v1 cannot tell which; a read of it is refused ERR_CHECKSUM, and it is "
                "published on the integrity tag\n", (unsigned long long)block,
                (unsigned long long)stored, (unsigned long long)read, (unsigned long long)cycle);
}

void IntegrityUndigestedName(uint64_t block)
{
    RotNaming naming = RotRemember(block);
    if (naming == ROT_KNOWN)
        return;
    RotPublish(block);
    if (naming == ROT_NEW)
        kprintf("[Integrity] block %llu carries no digest in its volume's Tally, and every block of "
                "format v2 is written with one — a read of it is refused ERR_CHECKSUM, and it is "
                "published on the integrity tag\n", (unsigned long long)block);
}

/* A block written again, or given back: what it holds now has a digest of its
 * own, and rot found in it later is new rot, named again. Nothing to look up
 * while the set is empty — the common case, and it costs one load; a long run
 * is looked up 64 blocks to a hold of the lock. */
void IntegrityRotForget(uint64_t start, uint32_t count)
{
    uint64_t end   = start + count;
    bool     named = __atomic_load_n(&g_rot_named, __ATOMIC_RELAXED) != 0;

    for (uint64_t b = start; b < end && named; ) {
        TagFSLockTake(&g_rot_lock);
        for (uint32_t n = 0; n < 64u && b < end && g_rot_named > 0; n++, b++)
            RotSetDrop(b);
        named = g_rot_named > 0;
        TagFSLockDrop(&g_rot_lock);
    }
}

void IntegrityRotEnd(void)
{
    TagFSLockTake(&g_rot_lock);
    uint64_t* old   = g_rot_set;
    uint32_t  named = g_rot_named;
    uint32_t  more  = g_rot_more;
    g_rot_set       = NULL;
    g_rot_slots     = 0;
    g_rot_named     = 0;
    g_rot_more      = 0;
    g_rot_tag       = TAGFS_INVALID_TAG_ID;
    TagFSLockDrop(&g_rot_lock);
    kfree(old);

    if (named || more)
        kprintf("[Integrity] this mount named %u rotten block(s), and %u more rot found that "
                "the set had no memory to name\n", named, more);
}
