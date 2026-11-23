#include "integrity_internal.h"
#include "../../lib/kernel/klib.h"
#include "touch.h"
#include "tagfs_volume.h"


typedef enum {
    ROT_NEW,      /* named now, for the first time this mount */
    ROT_KNOWN,    /* named before */
    ROT_NO_ROOM   /* the set could not grow to hold it */
} RotNaming;

static uint32_t RotSlotOf(uint64_t block, uint32_t slots)
{
    return (uint32_t)((block * 2654435761u) & (slots - 1u));
}

/* Under integrity_rot.rot_lock: the set holds `block`, or it is put in when there is room. */
static bool RotSetFind(TagFSVolume* vol, uint64_t block, bool put)
{
    if (vol->integrity_rot.rot_slots == 0)
        return false;
    uint64_t key = block + 1u;
    for (uint32_t i = RotSlotOf(block, vol->integrity_rot.rot_slots); ; i = (i + 1u) & (vol->integrity_rot.rot_slots - 1u)) {
        if (vol->integrity_rot.rot_set[i] == key)
            return true;
        if (vol->integrity_rot.rot_set[i] == 0) {
            if (put) {
                vol->integrity_rot.rot_set[i] = key;
                vol->integrity_rot.rot_named++;
            }
            return false;
        }
    }
}

/* The set's slots once more as many, the old table handed back to be freed
 * with the lock let go. Under integrity_rot.rot_lock. */
static uint64_t* RotSetTakeBigger(TagFSVolume* vol, uint64_t* bigger, uint32_t slots)
{
    uint64_t* old       = vol->integrity_rot.rot_set;
    uint32_t  old_slots = vol->integrity_rot.rot_slots;
    vol->integrity_rot.rot_set   = bigger;
    vol->integrity_rot.rot_slots = slots;
    vol->integrity_rot.rot_named = 0;
    for (uint32_t i = 0; i < old_slots; i++) {
        if (old[i])
            (void)RotSetFind(vol, old[i] - 1u, true);
    }
    return old;
}

static RotNaming RotRemember(TagFSVolume* vol, uint64_t block)
{
    for (;;) {
        TagFSLockTake(&vol->integrity_rot.rot_lock);
        if (RotSetFind(vol, block, false)) {
            TagFSLockDrop(&vol->integrity_rot.rot_lock);
            return ROT_KNOWN;
        }
        if ((vol->integrity_rot.rot_named + 1u) * 2u <= vol->integrity_rot.rot_slots) {
            (void)RotSetFind(vol, block, true);
            vol->integrity_rot.rot_room_said = false;
            TagFSLockDrop(&vol->integrity_rot.rot_lock);
            return ROT_NEW;
        }
        uint32_t slots = vol->integrity_rot.rot_slots;
        TagFSLockDrop(&vol->integrity_rot.rot_lock);

        uint32_t  want   = slots ? slots * 2u : 64u;
        uint64_t* bigger = want > slots ? kmalloc((size_t)want * sizeof(uint64_t)) : NULL;
        if (!bigger) {
            TagFSLockTake(&vol->integrity_rot.rot_lock);
            vol->integrity_rot.rot_more++;
            bool said       = vol->integrity_rot.rot_room_said;
            vol->integrity_rot.rot_room_said = true;
            TagFSLockDrop(&vol->integrity_rot.rot_lock);
            if (!said)
                kprintf("[Integrity] the set of rotten blocks named this mount holds %u and "
                        "has no memory to grow: rot found from here on is still published on "
                        "the integrity tag and answered to its reader, counted, and said when "
                        "the volume is let go\n", slots / 2u);
            return ROT_NO_ROOM;
        }
        memset(bigger, 0, (size_t)want * sizeof(uint64_t));

        uint64_t* old = NULL;
        TagFSLockTake(&vol->integrity_rot.rot_lock);
        if (vol->integrity_rot.rot_slots == slots)
            old = RotSetTakeBigger(vol, bigger, want);
        else
            old = bigger;          /* grown by another meanwhile: this one is not needed */
        TagFSLockDrop(&vol->integrity_rot.rot_lock);
        kfree(old);
    }
}

void IntegrityRotBegin(TagFSVolume* vol, uint32_t tag)
{
    TagFSLockTake(&vol->integrity_rot.rot_lock);
    uint64_t* old   = vol->integrity_rot.rot_set;
    vol->integrity_rot.rot_set       = NULL;
    vol->integrity_rot.rot_slots     = 0;
    vol->integrity_rot.rot_named     = 0;
    vol->integrity_rot.rot_more      = 0;
    vol->integrity_rot.rot_room_said = false;
    vol->integrity_rot.rot_tag       = tag;
    TagFSLockDrop(&vol->integrity_rot.rot_lock);
    kfree(old);
}

/* A block whose read is refused, published on the integrity tag. The event
 * carries the u32 block its readers are built for (box/touch.h — the
 * programs' side widens with wave 3): a block past one is never published cut
 * to another block's number; the line names it whole. */
static void RotPublish(TagFSVolume* vol, uint64_t block)
{
    uint32_t tag = __atomic_load_n(&vol->integrity_rot.rot_tag, __ATOMIC_RELAXED);
    if (tag != TAGFS_INVALID_TAG_ID && block <= UINT32_MAX) {
        struct __attribute__((packed)) {
            uint32_t block;
            uint8_t  op;        /* 3: a block that does not hold what was written to it */
            uint8_t  _pad[3];
        } ev = { (uint32_t)block, 3, { 0, 0, 0 } };
        TouchPublishId(tag, &ev, sizeof(ev), 0, TOUCH_FLAG_TAGFS);
    }
}

void IntegrityRotName(TagFSVolume* vol, uint64_t block, uint64_t stored, uint64_t read)
{
    RotNaming naming = RotRemember(vol, block);
    if (naming == ROT_KNOWN)
        return;
    RotPublish(vol, block);
    if (naming != ROT_NEW)
        return;

    /* The book writes no block in place: a closed block that
     * does not match is rot, whatever the last stop was. */
    kprintf("[Integrity] block %llu does not hold what was written to it (digest %016llx, "
            "read %016llx) — rot; a read of it is refused ERR_CHECKSUM, and it is "
            "published on the integrity tag\n", (unsigned long long)block,
            (unsigned long long)stored, (unsigned long long)read);
}

void IntegrityUndigestedName(TagFSVolume* vol, uint64_t block)
{
    RotNaming naming = RotRemember(vol, block);
    if (naming == ROT_KNOWN)
        return;
    RotPublish(vol, block);
    if (naming == ROT_NEW)
        kprintf("[Integrity] block %llu carries no digest in its volume's Tally, and every block of "
                "the book is written with one — a read of it is refused ERR_CHECKSUM, and it is "
                "published on the integrity tag\n", (unsigned long long)block);
}

void IntegrityRotEnd(TagFSVolume* vol)
{
    TagFSLockTake(&vol->integrity_rot.rot_lock);
    uint64_t* old   = vol->integrity_rot.rot_set;
    uint32_t  named = vol->integrity_rot.rot_named;
    uint32_t  more  = vol->integrity_rot.rot_more;
    vol->integrity_rot.rot_set       = NULL;
    vol->integrity_rot.rot_slots     = 0;
    vol->integrity_rot.rot_named     = 0;
    vol->integrity_rot.rot_more      = 0;
    vol->integrity_rot.rot_tag       = TAGFS_INVALID_TAG_ID;
    TagFSLockDrop(&vol->integrity_rot.rot_lock);
    kfree(old);

    if (named || more)
        kprintf("[Integrity] this mount named %u rotten block(s), and %u more rot found that "
                "the set had no memory to name\n", named, more);
}

/* The volume's integrity_rot part as it begins: what was not nothing. */
void IntegrityRotVolumeBegin(TagFSVolume* vol)
{
    vol->integrity_rot.rot_lock = (TagFSLock)TAGFS_LOCK_INIT("rot", LOCK_RANK_LEAF);
    vol->integrity_rot.rot_tag = TAGFS_INVALID_TAG_ID;
}
