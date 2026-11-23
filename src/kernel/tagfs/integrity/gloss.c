#include "gloss.h"
#include "tagfs_volume.h"

/*
 * Gloss (gloss.h): a tree of the volume's blocks, GLOSS_FAN slots a node, six
 * bits of a block number a level. A node of level 0 holds digests; a node above
 * it, the nodes one level down. The root covers [0, 64^height): a block past it
 * grows the tree by a root above the old one. A node whose last slot empties is
 * taken out of its parent and kept spare, and so up the tree.
 */

#define GLOSS_SHIFT      6u
#define GLOSS_FAN        (1u << GLOSS_SHIFT)
#define GLOSS_HEIGHT     11u    /* 6 * 11 = 66 bits: every u64 block */
#define GLOSS_SPARE_MOST 64u    /* spare nodes kept past a trim */

struct GlossNode {
    uint64_t present;              /* a bit a slot that holds */
    uint32_t level;                /* 0: its slots are digests */
    uint32_t count;                /* slots that hold */
    uint64_t slot[GLOSS_FAN];      /* a digest at level 0, else a GlossNode*; slot[0] chains a spare */
};

static uint32_t GlossIndex(uint64_t block, uint32_t level)
{
    uint32_t shift = GLOSS_SHIFT * level;
    return shift >= 64u ? 0u : (uint32_t)((block >> shift) & (GLOSS_FAN - 1u));
}

/* The fewest levels whose root covers `block`. */
static uint32_t GlossHeightFor(uint64_t block)
{
    uint32_t h = 1;
    while (h < GLOSS_HEIGHT && (block >> (GLOSS_SHIFT * h)) != 0)
        h++;
    return h;
}

static bool GlossCovers(TagFSVolume* vol, uint64_t block)
{
    return vol->gloss.gloss.root && (vol->gloss.gloss.height >= GLOSS_HEIGHT || (block >> (GLOSS_SHIFT * vol->gloss.gloss.height)) == 0);
}

// ----------------------------------------------------------------------------
// Room: nodes made before the lock
// ----------------------------------------------------------------------------

/*
 * The nodes a run of `count` blocks from `start` may want, counted for the
 * tallest tree there can be — another core's Ink may grow the root far above
 * the run between this count and the put: at every level its last block needs
 * the nodes its blocks span, and one node a level above them. The run's blocks
 * lie under one node of level h - 1 (h the levels its last block needs), so a
 * put takes at most one node a level on the path from the root down to it —
 * GLOSS_HEIGHT - 1 - h of them where the root stands far above it and nothing
 * else lies near, or h new roots where the tree is lower than the run; never
 * both, nor more than GLOSS_HEIGHT.
 */
static uint64_t GlossNodesFor(uint64_t start, uint64_t count)
{
    uint64_t end    = start + count - 1u;
    uint32_t height = GlossHeightFor(end);
    uint64_t nodes  = GLOSS_HEIGHT;
    for (uint32_t l = 0; l < height; l++) {
        uint32_t shift = GLOSS_SHIFT * (l + 1u);
        nodes += shift >= 64u ? 1u : (end >> shift) - (start >> shift) + 1u;
    }
    return nodes;
}

error_t GlossRoomFor(TagFSVolume* vol, const Strip* runs, uint32_t count, GlossRoom* room)
{
    uint64_t need = 0;
    room->nodes = NULL;
    room->count = 0;
    for (uint32_t i = 0; i < count; i++)
        if (runs[i].kind == STRIP_DATA && runs[i].count)
            need += GlossNodesFor(runs[i].start, runs[i].count);
    TagFSLockTake(&vol->gloss.gloss_lock);
    while (room->count < need && vol->gloss.gloss.spare) {
        GlossNode* n = vol->gloss.gloss.spare;
        vol->gloss.gloss.spare = (GlossNode*)(uintptr_t)n->slot[0];
        vol->gloss.gloss.nspare--;
        n->slot[0]  = (uint64_t)(uintptr_t)room->nodes;
        room->nodes = n;
        room->count++;
    }
    TagFSLockDrop(&vol->gloss.gloss_lock);
    while (room->count < need) {
        GlossNode* n = kmalloc(sizeof(GlossNode));
        if (!n)
            return ERR_NO_MEMORY;
        n->slot[0]  = (uint64_t)(uintptr_t)room->nodes;
        room->nodes = n;
        room->count++;
    }
    return OK;
}

void GlossRoomLetGo(TagFSVolume* vol, GlossRoom* room)
{
    GlossNode* n = room->nodes;
    TagFSLockTake(&vol->gloss.gloss_lock);
    while (n) {
        GlossNode* next = (GlossNode*)(uintptr_t)n->slot[0];
        n->slot[0]      = (uint64_t)(uintptr_t)vol->gloss.gloss.spare;
        vol->gloss.gloss.spare   = n;
        vol->gloss.gloss.nspare++;
        n = next;
    }
    TagFSLockDrop(&vol->gloss.gloss_lock);
    room->nodes = NULL;
    room->count = 0;
    GlossTrim(vol);
}

/* A node of `level` from the room, empty; NULL — said once, a defect in a
 * STRICT kernel — when the room is short of the count it was made for. */
static GlossNode* GlossNodeTaken(TagFSVolume* vol, GlossRoom* room, uint32_t level)
{
    GlossNode* n = room->nodes;
    if (!n) {
#if CONFIG_STRICT
        panic("[Gloss] DEFECT: a digest was put in with no node left of the room made for it");
#endif
        if (!vol->gloss.gloss.short_said)
            kprintf("[Gloss] DEFECT: a digest was put in with no node left of the room made for it — the block "
                    "it names is refused ERR_CHECKSUM when read\n");
        vol->gloss.gloss.short_said = true;
        return NULL;
    }
    room->nodes = (GlossNode*)(uintptr_t)n->slot[0];
    room->count--;
    memset(n, 0, sizeof(*n));
    n->level = level;
    return n;
}

// ----------------------------------------------------------------------------
// Put, find, copy
// ----------------------------------------------------------------------------

/* The tree grown to cover `block`: false when the room was short. */
static bool GlossGrow(TagFSVolume* vol, GlossRoom* room, uint64_t block)
{
    if (!vol->gloss.gloss.root) {
        uint32_t height = GlossHeightFor(block);
        if (!(vol->gloss.gloss.root = GlossNodeTaken(vol, room, height - 1u)))
            return false;
        vol->gloss.gloss.height = height;
    }
    while (!GlossCovers(vol, block)) {
        GlossNode* up = GlossNodeTaken(vol, room, vol->gloss.gloss.height);
        if (!up)
            return false;
        up->slot[0]  = (uint64_t)(uintptr_t)vol->gloss.gloss.root;
        up->present  = 1u;
        up->count    = 1u;
        vol->gloss.gloss.root = up;
        vol->gloss.gloss.height++;
    }
    return true;
}

void GlossPut(TagFSVolume* vol, GlossRoom* room, uint64_t block, uint64_t digest)
{
    TagFSLockTake(&vol->gloss.gloss_lock);
    GlossNode* n = GlossGrow(vol, room, block) ? vol->gloss.gloss.root : NULL;
    for (uint32_t l = vol->gloss.gloss.height - 1u; n && l > 0; l--) {
        uint32_t i = GlossIndex(block, l);
        if (!(n->present & (1ull << i))) {
            GlossNode* kid = GlossNodeTaken(vol, room, l - 1u);
            if (!kid) {
                n = NULL;
                break;
            }
            n->slot[i]  = (uint64_t)(uintptr_t)kid;
            n->present |= 1ull << i;
            n->count++;
        }
        n = (GlossNode*)(uintptr_t)n->slot[i];
    }
    if (n) {
        uint32_t i = GlossIndex(block, 0);
        if (!(n->present & (1ull << i))) {
            n->present |= 1ull << i;
            n->count++;
            vol->gloss.gloss.entries++;
        }
        n->slot[i] = digest;
    }
    TagFSLockDrop(&vol->gloss.gloss_lock);
}

error_t GlossNote(TagFSVolume* vol, uint64_t block, uint64_t digest)
{
    Strip     one  = { .start = block, .count = 1, .kind = STRIP_DATA };
    GlossRoom room;
    error_t   rc = GlossRoomFor(vol, &one, 1, &room);
    if (rc == OK)
        GlossPut(vol, &room, block, digest);
    GlossRoomLetGo(vol, &room);
    return rc;
}

/* Under the lock: the leaf holding `block`, or NULL. */
static const GlossNode* GlossLeafLocked(TagFSVolume* vol, uint64_t block)
{
    const GlossNode* n = GlossCovers(vol, block) ? vol->gloss.gloss.root : NULL;
    for (uint32_t l = vol->gloss.gloss.height - 1u; n && l > 0; l--) {
        uint32_t i = GlossIndex(block, l);
        n = n->present & (1ull << i) ? (const GlossNode*)(uintptr_t)n->slot[i] : NULL;
    }
    return n;
}

bool GlossFind(TagFSVolume* vol, uint64_t block, uint64_t* digest)
{
    TagFSLockTake(&vol->gloss.gloss_lock);
    const GlossNode* leaf = GlossLeafLocked(vol, block);
    uint32_t         i    = GlossIndex(block, 0);
    bool             held = leaf && (leaf->present & (1ull << i));
    if (held)
        *digest = leaf->slot[i];
    TagFSLockDrop(&vol->gloss.gloss_lock);
    return held;
}

bool GlossCopy(TagFSVolume* vol, uint64_t start, uint64_t count, uint8_t* out)
{
    for (uint64_t b = start; b < start + count;) {
        uint64_t stop = (b | (GLOSS_FAN - 1u)) + 1u;
        if (stop > start + count)
            stop = start + count;
        TagFSLockTake(&vol->gloss.gloss_lock);
        const GlossNode* leaf = GlossLeafLocked(vol, b);
        bool             held = leaf != NULL;
        for (; held && b < stop; b++, out += 8) {
            uint32_t i = GlossIndex(b, 0);
            held = (leaf->present & (1ull << i)) != 0;
            if (held)
                memcpy(out, &leaf->slot[i], 8);
        }
        TagFSLockDrop(&vol->gloss.gloss_lock);
        if (!held)
            return false;
    }
    return true;
}

// ----------------------------------------------------------------------------
// Forget, trim, let go
// ----------------------------------------------------------------------------

/* Under the lock: `n` emptied, kept spare. */
static void GlossSpareLocked(TagFSVolume* vol, GlossNode* n)
{
    n->slot[0]    = (uint64_t)(uintptr_t)vol->gloss.gloss.spare;
    vol->gloss.gloss.spare = n;
    vol->gloss.gloss.nspare++;
}

/* Under the lock: the digest of `block` taken out, and every node it leaves
 * empty with it. The blocks past `block` the walk found no node for: *skip. */
static void GlossForgetOneLocked(TagFSVolume* vol, uint64_t block, uint64_t* skip)
{
    GlossNode* path[GLOSS_HEIGHT];
    uint32_t   at[GLOSS_HEIGHT];
    GlossNode* n = vol->gloss.gloss.root;
    uint32_t   l = vol->gloss.gloss.height - 1u;
    *skip = 1;
    for (;; l--) {
        uint32_t i = GlossIndex(block, l);
        path[l] = n;
        at[l]   = i;
        if (!(n->present & (1ull << i))) {
            uint32_t shift = GLOSS_SHIFT * l;
            *skip = shift >= 64u ? ~0ull : (1ull << shift) - (block & ((1ull << shift) - 1u));
            return;
        }
        if (l == 0)
            break;
        n = (GlossNode*)(uintptr_t)n->slot[i];
    }
    for (l = 0; l < vol->gloss.gloss.height; l++) {
        n = path[l];
        n->present &= ~(1ull << at[l]);
        n->count--;
        if (l == 0)
            vol->gloss.gloss.entries--;
        if (n->count != 0)
            return;
        GlossSpareLocked(vol, n);
    }
    vol->gloss.gloss.root   = NULL;
    vol->gloss.gloss.height = 0;
}

void GlossForget(TagFSVolume* vol, uint64_t start, uint64_t count)
{
    TagFSLockTake(&vol->gloss.gloss_lock);
    for (uint64_t b = start; b < start + count && vol->gloss.gloss.root;) {
        uint64_t skip = 1;
        if (GlossCovers(vol, b))
            GlossForgetOneLocked(vol, b, &skip);
        else
            break;
        b = skip > start + count - b ? start + count : b + skip;
    }
    TagFSLockDrop(&vol->gloss.gloss_lock);
}

void GlossTrim(TagFSVolume* vol)
{
    GlossNode* gone = NULL;
    TagFSLockTake(&vol->gloss.gloss_lock);
    while (vol->gloss.gloss.nspare > GLOSS_SPARE_MOST) {
        GlossNode* n  = vol->gloss.gloss.spare;
        vol->gloss.gloss.spare = (GlossNode*)(uintptr_t)n->slot[0];
        vol->gloss.gloss.nspare--;
        n->slot[0] = (uint64_t)(uintptr_t)gone;
        gone       = n;
    }
    TagFSLockDrop(&vol->gloss.gloss_lock);
    while (gone) {
        GlossNode* next = (GlossNode*)(uintptr_t)gone->slot[0];
        kfree(gone);
        gone = next;
    }
}

void GlossLetGo(TagFSVolume* vol)
{
    GlossNode* path[GLOSS_HEIGHT];
    uint32_t   at[GLOSS_HEIGHT];
    uint32_t   depth = 0;
    TagFSLockTake(&vol->gloss.gloss_lock);
    GlossNode* root  = vol->gloss.gloss.root;
    GlossNode* spare = vol->gloss.gloss.spare;
    memset(&vol->gloss.gloss, 0, sizeof(vol->gloss.gloss));
    TagFSLockDrop(&vol->gloss.gloss_lock);
    if (root) {
        path[0] = root;
        at[0]   = 0;
        depth   = 1;
    }
    while (depth) {
        GlossNode* n = path[depth - 1u];
        if (n->level == 0 || at[depth - 1u] == GLOSS_FAN) {
            kfree(n);
            depth--;
            continue;
        }
        uint32_t i = at[depth - 1u]++;
        if (n->present & (1ull << i)) {
            path[depth] = (GlossNode*)(uintptr_t)n->slot[i];
            at[depth]   = 0;
            depth++;
        }
    }
    while (spare) {
        GlossNode* next = (GlossNode*)(uintptr_t)spare->slot[0];
        kfree(spare);
        spare = next;
    }
}

#ifdef CONFIG_VOLUME_TESTS
uint64_t GlossStandCount(TagFSVolume* vol)
{
    TagFSLockTake(&vol->gloss.gloss_lock);
    uint64_t n = vol->gloss.gloss.entries;
    TagFSLockDrop(&vol->gloss.gloss_lock);
    return n;
}
#endif

/* The volume's gloss part as it begins: what was not nothing. */
void GlossVolumeBegin(TagFSVolume* vol)
{
    vol->gloss.gloss_lock = (TagFSLock)TAGFS_LOCK_INIT("gloss", LOCK_RANK_LEAF);
}
