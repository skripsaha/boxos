#include "block_map_derived.h"
#include "block_map.h"            /* AllotmentTakeBackLocked — a window given back before the ribbon is refused */
#include "../tagfs.h"
#include "../ink/fallow.h"    /* FallowBarsRun — ground a Tally still names, kept from the ribbon's Quire */
#include "amp.h"           /* amp_get_core_index — the stand's HEAPFAIL on the builder's core */
#include "tagfs_volume.h"

/*
 * The derived map (block_map_derived.h). A window's state is what its summary
 * says: a page — mixed; no page — whole free when its count is its blocks,
 * whole taken when it is 0. A change that leaves a window whole lets its page
 * go; one that leaves it mixed makes the page first, the edges of a run being
 * the only windows a run can leave so, so that nothing changes unless all of
 * the run can.
 */

#define DERIVED_WORDS  (BLOCK_MAP_WINDOW_BLOCKS / 64u)
#define DERIVED_NAMED  32u   /* claims and releases that meet a block in the wrong state, named one
                              * by one — the owner's bound for a list said (09-27) */

_Static_assert(sizeof(DerivedWindow) == 16, "a window's summary is sixteen bytes");

#ifdef CONFIG_VOLUME_TESTS
static uint32_t s_stand_refuse;
static uint32_t s_stand_short;
#endif

// ----------------------------------------------------------------------------
// Pages
// ----------------------------------------------------------------------------

/* The window's blocks the volume has: all of them but in the last window. */
static uint32_t WindowBlocks(TagFSVolume* vol, uint32_t w)
{
    uint64_t base = (uint64_t)w * BLOCK_MAP_WINDOW_BLOCKS;
    return vol->block_map_derived.map.end - base < BLOCK_MAP_WINDOW_BLOCKS ? (uint32_t)(vol->block_map_derived.map.end - base) : BLOCK_MAP_WINDOW_BLOCKS;
}

/* Set bits in a word, counted here: GCC lowers __builtin_popcountll to a
 * libgcc call on this target, and the kernel links no libgcc. */
static uint32_t WordOnes(uint64_t x)
{
    uint32_t n = 0;
    for (; x; x &= x - 1u)
        n++;
    return n;
}

/* The longest run of clear bits in a page. */
static uint32_t BitsLongest(const uint64_t* bits)
{
    uint32_t best = 0, run = 0;
    for (uint32_t i = 0; i < DERIVED_WORDS; i++) {
        uint64_t x = bits[i];
        if (x == 0) {
            run += 64u;
            continue;
        }
        for (uint32_t b = 0; b < 64u; b++) {
            if (x & (1ull << b)) {
                best = run > best ? run : best;
                run  = 0;
            } else {
                run++;
            }
        }
    }
    return run > best ? run : best;
}

/* Bits [from, to) of a page set (`take`) or cleared: how many were so already. */
static uint32_t BitsPut(uint64_t* bits, uint32_t from, uint32_t to, bool take)
{
    uint32_t already = 0;
    while (from < to) {
        uint32_t  b    = from % 64u;
        uint32_t  n    = to - from < 64u - b ? to - from : 64u - b;
        uint64_t  mask = (n == 64u ? ~0ull : ((1ull << n) - 1u)) << b;
        uint64_t* word = &bits[from / 64u];
        already += WordOnes(take ? (*word & mask) : (~*word & mask));
        *word = take ? (*word | mask) : (*word & ~mask);
        from += n;
    }
    return already;
}

/* A page for the map: the heap's while it is built, with the door shut and no
 * lock held; once it is up, the stock's — made before the state lock its
 * changes are taken under (BlockMapDerivedStock), which no allocation may be
 * asked under (model A). NULL when there is none. */
static uint64_t* PageTaken(TagFSVolume* vol)
{
    if (vol->block_map_derived.map.building)
        return kmalloc(DERIVED_WORDS * sizeof(uint64_t));
    for (uint32_t i = 0; i < DERIVED_STOCK; i++) {
        uint64_t* page = vol->block_map_derived.map.stock[i];
        if (page) {
            vol->block_map_derived.map.stock[i] = NULL;
            return page;
        }
    }
    uint64_t* page = vol->block_map_derived.map.held;
    if (page) {
        vol->block_map_derived.map.held = (uint64_t*)(uintptr_t)page[0];
        vol->block_map_derived.map.nheld--;
    }
    return page;
}

/* A page the map no longer wants: the heap's again while it is built, with no
 * lock held; once it is up — under the state lock, which nothing is given back
 * to the heap under (model A; fallow.h, block_map.c alike) — the stock's when
 * it has room, else chained loose for the change's maker to free once the
 * lock is dropped (PagesLoose). */
static void PageGiven(TagFSVolume* vol, uint64_t* page)
{
    if (vol->block_map_derived.map.building) {
        kfree(page);
        return;
    }
    for (uint32_t i = 0; i < DERIVED_STOCK; i++) {
        if (!vol->block_map_derived.map.stock[i]) {
            vol->block_map_derived.map.stock[i] = page;
            return;
        }
    }
    page[0]     = (uint64_t)(uintptr_t)vol->block_map_derived.map.loose;
    vol->block_map_derived.map.loose = page;
}

/* The pages a change of the live map let go of, taken under the state lock. */
static uint64_t* PagesLooseTaken(TagFSVolume* vol)
{
    uint64_t* loose = vol->block_map_derived.map.loose;
    vol->block_map_derived.map.loose     = NULL;
    return loose;
}

/* ...and freed once it is dropped. */
static void PagesLoose(uint64_t* page)
{
    while (page) {
        uint64_t* next = (uint64_t*)(uintptr_t)page[0];
        kfree(page);
        page = next;
    }
}

/* A page for window w as its summary says it is — every block clear when it
 * is whole free, every one set when whole taken — and the blocks past the
 * map's end set: no take may have them. NULL when there is no page to make it. */
static uint64_t* PageMade(TagFSVolume* vol, uint32_t w)
{
#ifdef CONFIG_VOLUME_TESTS
    if (vol->block_map_derived.map.building && __atomic_exchange_n(&s_stand_refuse, 0u, __ATOMIC_ACQ_REL))
        HeapFailArm(amp_get_core_index(), 1);
#endif
    uint64_t* bits = PageTaken(vol);
    if (!bits)
        return NULL;
    memset(bits, vol->block_map_derived.map.win[w].free ? 0x00 : 0xFF, DERIVED_WORDS * sizeof(uint64_t));
    BitsPut(bits, WindowBlocks(vol, w), BLOCK_MAP_WINDOW_BLOCKS, true);
    return bits;
}

/* The summary after the window changed: a window left whole lets its page go.
 * Its longest run is found again — but not while the map is built: nothing
 * asks for a run then, and a mixed window takes as many claims as it has
 * gaps, so each would look at the whole page once more. The build's end finds
 * every one once (BlockMapDerivedBuilt). */
static void WindowSettle(TagFSVolume* vol, uint32_t w)
{
    DerivedWindow* win = &vol->block_map_derived.map.win[w];
    if (win->bits && (win->free == 0 || win->free == WindowBlocks(vol, w))) {
        PageGiven(vol, win->bits);
        win->bits = NULL;
    }
    if (!win->bits)
        win->longest = win->free;
    else if (!vol->block_map_derived.map.building)
        win->longest = BitsLongest(win->bits);
}

// ----------------------------------------------------------------------------
// A run put
// ----------------------------------------------------------------------------

/* Window w's part of [start, end) as [*from, *to) of the window. */
static void WindowPart(TagFSVolume* vol, uint32_t w, uint64_t start, uint64_t end, uint32_t* from, uint32_t* to)
{
    uint64_t base = (uint64_t)w * BLOCK_MAP_WINDOW_BLOCKS;
    uint32_t n    = WindowBlocks(vol, w);
    *from = start > base ? (uint32_t)(start - base) : 0;
    *to   = end - base < n ? (uint32_t)(end - base) : n;
}

/* Whether the run leaves window w mixed and it has no page to say so: a part
 * of it put to the state the whole window is not in. */
static bool WindowWantsPage(TagFSVolume* vol, uint32_t w, uint64_t start, uint64_t end, bool take)
{
    const DerivedWindow* win = &vol->block_map_derived.map.win[w];
    uint32_t             n   = WindowBlocks(vol, w), from, to;
    WindowPart(vol, w, start, end, &from, &to);
    return !win->bits && (from > 0 || to < n) && (take ? win->free == n : win->free == 0);
}

/* The run's part in window w put: how many of its blocks were so already. A
 * window with no page whose part is in the state asked already is left as it
 * is; one with none while the build is short of pages is counted alone. */
static uint32_t WindowPut(TagFSVolume* vol, uint32_t w, uint64_t start, uint64_t end, bool take)
{
    DerivedWindow* win = &vol->block_map_derived.map.win[w];
    uint32_t       n   = WindowBlocks(vol, w), was = win->free, from, to, met = 0;
    WindowPart(vol, w, start, end, &from, &to);
    if (from == 0 && to == n) {
        met       = take ? n - win->free : win->free;
        win->free = take ? 0 : n;
    } else if (win->bits) {
        met       = BitsPut(win->bits, from, to, take);
        win->free = take ? win->free - (to - from - met) : win->free + (to - from - met);
    } else if (take ? win->free == 0 : win->free == n) {
        met = to - from;
    } else if (take) {
        win->free = win->free > to - from ? win->free - (to - from) : 0;
    } else {
        win->free = n - win->free > to - from ? win->free + (to - from) : n;
    }
    vol->block_map_derived.map.free = vol->block_map_derived.map.free - was + win->free;
    WindowSettle(vol, w);
    return met;
}

/* [start, end) put to `take`: the pages its two edges need made first — a
 * run's inner windows it covers whole — so that nothing changes unless all of
 * it can; *met how many were so already. ERR_NO_MEMORY, nothing changed, for a
 * page refused once the map is up; while it is built, the build counts on. */
static error_t RunPut(TagFSVolume* vol, uint64_t start, uint64_t end, bool take, uint64_t* met)
{
    uint32_t w0 = (uint32_t)(start / BLOCK_MAP_WINDOW_BLOCKS);
    uint32_t w1 = (uint32_t)((end - 1u) / BLOCK_MAP_WINDOW_BLOCKS);
    bool     made0 = false;

    for (uint32_t k = 0; k < 2u && !vol->block_map_derived.map.short_of; k++) {
        uint32_t w = k ? w1 : w0;
        if ((k && w1 == w0) || !WindowWantsPage(vol, w, start, end, take))
            continue;
        if ((vol->block_map_derived.map.win[w].bits = PageMade(vol, w)) != NULL) {
            made0 = made0 || k == 0;
            continue;
        }
        if (made0) {
            PageGiven(vol, vol->block_map_derived.map.win[w0].bits);
            vol->block_map_derived.map.win[w0].bits = NULL;
        }
        if (!vol->block_map_derived.map.building)
            return ERR_NO_MEMORY;
        vol->block_map_derived.map.short_of = true;
    }
    for (uint32_t w = w0; w <= w1; w++)
        *met += WindowPut(vol, w, start, end, take);
    return OK;
}

/* A claim that met blocks taken already, or a release that met blocks free:
 * the first DERIVED_NAMED named, the rest counted — and the first of the rest
 * says that they are, so a machine halted before the map is let go has said
 * there were more. */
static void MetSay(TagFSVolume* vol, GroundOwner owner, uint64_t who, uint64_t start, uint64_t count, uint64_t met, bool take)
{
    char by[48];
    if (vol->block_map_derived.map.named >= DERIVED_NAMED) {
        if (vol->block_map_derived.map.unnamed++ == 0)
            kprintf("[TagFS FSCK] more claims and releases on the block map meet blocks so already than are "
                    "named one by one (%u) — from here they are counted, and the count said when the map is "
                    "let go\n", DERIVED_NAMED);
        return;
    }
    vol->block_map_derived.map.named++;
    if (owner == GROUND_FILE)
        ksnprintf(by, sizeof(by), "file %llu", (unsigned long long)who);
    else
        ksnprintf(by, sizeof(by), "the %s", GroundOwnerName(owner));
    if (take)
        kprintf("[TagFS FSCK] the run of %llu block(s) from data block %llu that %s claims meets %llu taken "
                "already — two owners name them\n", (unsigned long long)count, (unsigned long long)start, by,
                (unsigned long long)met);
    else
        kprintf("[TagFS FSCK] the run of %llu block(s) from data block %llu that %s lets go meets %llu free "
                "already — a block freed twice is how one block comes to have two owners\n",
                (unsigned long long)count, (unsigned long long)start, by, (unsigned long long)met);
}

static error_t MapPut(TagFSVolume* vol, GroundOwner owner, uint64_t who, uint64_t start, uint64_t count, bool take, uint64_t* met)
{
    *met = 0;
    if (!vol->block_map_derived.map.win)
        return ERR_NOT_INITIALIZED;
    uint64_t end = count > vol->block_map_derived.map.end - (start < vol->block_map_derived.map.end ? start : vol->block_map_derived.map.end) ? vol->block_map_derived.map.end : start + count;
    if (start >= end)
        return OK;
    error_t rc = RunPut(vol, start, end, take, met);
    if (rc == OK && *met)
        MetSay(vol, owner, who, start, end - start, *met, take);
    if (vol->block_map_derived.map.up)
        vol->state.free_blocks = vol->block_map_derived.map.free;
    return rc;
}

error_t BlockMapDerivedClaim(TagFSVolume* vol, GroundOwner owner, uint64_t who, uint64_t start, uint64_t count, uint64_t* taken)
{
    return MapPut(vol, owner, who, start, count, true, taken);
}

error_t BlockMapDerivedRelease(TagFSVolume* vol, GroundOwner owner, uint64_t who, uint64_t start, uint64_t count, uint64_t* free)
{
    return MapPut(vol, owner, who, start, count, false, free);
}

// ----------------------------------------------------------------------------
// The build, the questions, letting go
// ----------------------------------------------------------------------------

error_t BlockMapDerivedBegin(TagFSVolume* vol, uint64_t end, const char** what)
{
    BlockMapDerivedLetGo(vol);
    uint64_t windows = (end + BLOCK_MAP_WINDOW_BLOCKS - 1u) / BLOCK_MAP_WINDOW_BLOCKS;
    if (windows > UINT32_MAX) {
        ksnprintf(vol->block_map_derived.words, sizeof(vol->block_map_derived.words), "its block map would count %llu windows of %u blocks, and it counts "
                  "them in 32 bits", (unsigned long long)windows, BLOCK_MAP_WINDOW_BLOCKS);
        *what = vol->block_map_derived.words;
        return ERR_OUT_OF_RANGE;
    }
    vol->block_map_derived.map.win = kmalloc(sizeof(DerivedWindow) * windows);
    if (!vol->block_map_derived.map.win) {
        ksnprintf(vol->block_map_derived.words, sizeof(vol->block_map_derived.words), "there is no memory for its block map's summary: %llu windows, "
                  "%llu KiB", (unsigned long long)windows,
                  (unsigned long long)(windows * sizeof(DerivedWindow) / 1024u));
        *what = vol->block_map_derived.words;
        return ERR_NO_MEMORY;
    }
    vol->block_map_derived.map.end      = end;
    vol->block_map_derived.map.windows  = (uint32_t)windows;
    vol->block_map_derived.map.free     = end;
    vol->block_map_derived.map.building = true;
    for (uint32_t w = 0; w < vol->block_map_derived.map.windows; w++)
        vol->block_map_derived.map.win[w] = (DerivedWindow){ WindowBlocks(vol, w), WindowBlocks(vol, w), NULL };
    return OK;
}

/* Windows both free and taken. */
static uint32_t MapMixed(TagFSVolume* vol)
{
    uint32_t mixed = 0;
    for (uint32_t w = 0; w < vol->block_map_derived.map.windows; w++)
        mixed += vol->block_map_derived.map.win[w].free > 0 && vol->block_map_derived.map.win[w].free < WindowBlocks(vol, w);
    return mixed;
}

error_t BlockMapDerivedBuilt(TagFSVolume* vol, const char** what)
{
    vol->block_map_derived.map.building = false;
    if (vol->block_map_derived.map.short_of) {
        uint32_t mixed = MapMixed(vol);
        ksnprintf(vol->block_map_derived.words, sizeof(vol->block_map_derived.words), "its block map is short of memory: %u mixed window(s) want %llu "
                  "bytes of pages past the %llu bytes of its summary", mixed,
                  (unsigned long long)mixed * (DERIVED_WORDS * sizeof(uint64_t)),
                  (unsigned long long)vol->block_map_derived.map.windows * sizeof(DerivedWindow));
#ifdef CONFIG_VOLUME_TESTS
        s_stand_short = mixed;
#endif
        BlockMapDerivedLetGo(vol);
        *what = vol->block_map_derived.words;
        return ERR_NO_MEMORY;
    }
    for (uint32_t w = 0; w < vol->block_map_derived.map.windows; w++)
        if (vol->block_map_derived.map.win[w].bits)
            vol->block_map_derived.map.win[w].longest = BitsLongest(vol->block_map_derived.map.win[w].bits);
    vol->block_map_derived.map.up = true;
    vol->state.free_blocks = vol->block_map_derived.map.free;
    return OK;
}

bool BlockMapDerivedUp(TagFSVolume* vol)
{
    return vol->block_map_derived.map.up;
}

bool BlockMapDerivedIsFree(TagFSVolume* vol, uint64_t block)
{
    if (!vol->block_map_derived.map.win || block >= vol->block_map_derived.map.end)
        return false;
    const DerivedWindow* win = &vol->block_map_derived.map.win[block / BLOCK_MAP_WINDOW_BLOCKS];
    uint32_t             off = (uint32_t)(block % BLOCK_MAP_WINDOW_BLOCKS);
    return win->bits ? !(win->bits[off / 64u] & (1ull << (off % 64u))) : win->free != 0;
}

bool BlockMapDerivedWindow(TagFSVolume* vol, uint32_t w, uint32_t* blocks, uint32_t* free, const uint64_t** bits)
{
    if (!vol->block_map_derived.map.up || w >= vol->block_map_derived.map.windows)
        return false;
    *blocks = WindowBlocks(vol, w);
    *free   = vol->block_map_derived.map.win[w].free;
    *bits   = vol->block_map_derived.map.win[w].bits;
    return true;
}

bool BlockMapDerivedRunIn(TagFSVolume* vol, uint32_t window, uint32_t want, uint64_t* start)
{
    if (!vol->block_map_derived.map.win || window >= vol->block_map_derived.map.windows || want == 0 || vol->block_map_derived.map.win[window].longest < want)
        return false;
    const DerivedWindow* win  = &vol->block_map_derived.map.win[window];
    uint64_t             base = (uint64_t)window * BLOCK_MAP_WINDOW_BLOCKS;
    uint32_t             run  = 0;
    if (!win->bits) {
        *start = base;
        return true;
    }
    for (uint32_t off = 0; off < BLOCK_MAP_WINDOW_BLOCKS; off++) {
        run = win->bits[off / 64u] & (1ull << (off % 64u)) ? 0 : run + 1u;
        if (run == want) {
            *start = base + off + 1u - want;
            return true;
        }
    }
    return false;
}

// ----------------------------------------------------------------------------
// The live map: its stock of pages, and the ribbon's Quire
// ----------------------------------------------------------------------------

error_t BlockMapDerivedStock(TagFSVolume* vol)
{
    TagFSLock* lock = &vol->state.lock;
    for (uint32_t i = 0; i < DERIVED_STOCK; i++) {
        TagFSLockTake(lock);
        bool full = vol->block_map_derived.map.stock[i] != NULL;
        TagFSLockDrop(lock);
        if (full)
            continue;
        uint64_t* page = kmalloc(DERIVED_WORDS * sizeof(uint64_t));
        if (!page)
            return ERR_NO_MEMORY;
        TagFSLockTake(lock);
        if (!vol->block_map_derived.map.stock[i]) {
            vol->block_map_derived.map.stock[i] = page;
            page           = NULL;
        }
        TagFSLockDrop(lock);
        kfree(page);
    }
    return OK;
}

/*
 * ‼ THE PAGES FALLOW'S GIVES MAY WANT ARE HELD BEFORE THE LOCK THEY ARE GIVEN
 * UNDER (model A, §16.2 п.16). A cycle's DONE gives back every node that
 * ripened, under the state lock, where the heap is asked for nothing; a run it
 * gives back leaves a window mixed only at its two ends. So the cycle's TAKE
 * holds what its gives may want — the windows each run of a node that may
 * ripen reaches (FallowGiveRoomHold) — as pages chained apart from the stock,
 * drawn on once the stock is spent: the held made `pages` exactly, those past
 * it let go, and with them the pages earlier changes let go loose under the
 * lock. A give a page is still short for leaves its run resting (fallow.c,
 * NodeGiveBack), and the next cycle gives it.
 */
error_t BlockMapDerivedHold(TagFSVolume* vol, uint32_t pages)
{
    TagFSLock* lock  = &vol->state.lock;
    uint64_t*  spare = NULL;
    TagFSLockTake(lock);
    if (!vol->block_map_derived.map.up)
        pages = 0;
    if (pages > vol->block_map_derived.map.windows)
        pages = vol->block_map_derived.map.windows;      /* a window is made mixed once */
    while (vol->block_map_derived.map.nheld > pages) {
        uint64_t* page = vol->block_map_derived.map.held;
        vol->block_map_derived.map.held = (uint64_t*)(uintptr_t)page[0];
        vol->block_map_derived.map.nheld--;
        page[0] = (uint64_t)(uintptr_t)spare;
        spare   = page;
    }
    uint64_t* loose = PagesLooseTaken(vol);
    uint32_t  have  = vol->block_map_derived.map.nheld;
    TagFSLockDrop(lock);
    PagesLoose(spare);
    PagesLoose(loose);
    while (have < pages) {
        uint64_t* page = kmalloc(DERIVED_WORDS * sizeof(uint64_t));
        if (!page)
            return ERR_NO_MEMORY;
        TagFSLockTake(lock);
        page[0]    = (uint64_t)(uintptr_t)vol->block_map_derived.map.held;
        vol->block_map_derived.map.held = page;
        have       = ++vol->block_map_derived.map.nheld;
        TagFSLockDrop(lock);
    }
    return OK;
}

/* The run so far, `run` free blocks from `at`, grown by the free block or
 * blocks [b, b + n): a run begins only on `align`. Whether it reaches `want`. */
static bool RunGrows(uint64_t b, uint64_t n, uint32_t align, uint64_t want, uint64_t* at, uint64_t* run)
{
    if (*run == 0) {
        uint64_t first = (b + align - 1u) / align * align;
        if (first >= b + n)
            return false;
        *at  = first;
        *run = b + n - first;
    } else {
        *run += n;
    }
    return *run >= want;
}

/* The first run of `want` free blocks beginning on `align` inside [lo, hi):
 * *start. A window whole free or whole taken is passed by its summary, and
 * one of both is read a word of its bits at a time. */
static bool RunAlignedIn(TagFSVolume* vol, uint64_t lo, uint64_t hi, uint64_t want, uint32_t align, uint64_t* start)
{
    uint64_t run = 0, at = 0;
    for (uint64_t b = lo; b < hi;) {
        uint32_t             w    = (uint32_t)(b / BLOCK_MAP_WINDOW_BLOCKS);
        uint64_t             base = (uint64_t)w * BLOCK_MAP_WINDOW_BLOCKS;
        uint64_t             end  = base + WindowBlocks(vol, w) < hi ? base + WindowBlocks(vol, w) : hi;
        const DerivedWindow* win  = &vol->block_map_derived.map.win[w];
        if (!win->bits) {
            if (win->free == 0)
                run = 0;
            else if (RunGrows(b, end - b, align, want, &at, &run))
                break;
            b = end;
            continue;
        }
        /* A stretch at a time, read off a word of the bits at once: the
         * blocks taken from here, or those free, as far as the word goes —
         * the very run a block at a time finds, a free stretch growing it as
         * its blocks one by one would. */
        while (b < end) {
            uint32_t off   = (uint32_t)(b - base);
            uint32_t shift = off % 64u;
            uint64_t word  = win->bits[off / 64u] >> shift;
            uint64_t left  = 64u - shift < end - b ? 64u - shift : end - b;
            if (word & 1u) {
                uint64_t taken = ~word ? (uint64_t)__builtin_ctzll(~word) : 64u;
                run = 0;
                b += taken < left ? taken : left;
                continue;
            }
            uint64_t free = word ? (uint64_t)__builtin_ctzll(word) : 64u;
            if (free > left) free = left;
            if (RunGrows(b, free, align, want, &at, &run))
                break;
            b += free;
        }
        if (run >= want)
            break;
    }
    if (run < want || at + want > hi)
        return false;
    *start = at;
    return true;
}

/* The first run of `want` free blocks beginning on `align` inside [lo, hi)
 * that lies in no file's window but `fid`'s (block_map.h, Allotment; 0: in no
 * file's at all): *start. A run a window cuts is looked for again past it. */
static bool RunAlignedFree(TagFSVolume* vol, uint64_t lo, uint64_t hi, uint64_t want, uint32_t align, uint64_t fid,
                           uint64_t* start)
{
    uint64_t past = 0;
    while (lo < hi && RunAlignedIn(vol, lo, hi, want, align, start)) {
        if (!AllotmentOthersIn(vol, fid, *start, want, &past))
            return true;
        lo = past > *start ? past : *start + 1u;
    }
    return false;
}

/* RunAlignedFree for a run of the book's own: for the ribbon's Quire, past
 * the ground Fallow keeps from the ribbon (fallow.h, FallowBarsRun) — a
 * Tally a mount may stand on names a file on it, and a reader would refuse the
 * ribbon's step there; *barred when a run passed over for it was free. */
static bool RunQuireFree(TagFSVolume* vol, GroundOwner owner, uint64_t lo, uint64_t hi, uint64_t want, uint32_t align,
                         uint64_t* start, bool* barred)
{
    uint64_t past = 0;
    while (lo < hi && RunAlignedFree(vol, lo, hi, want, align, 0, start)) {
        if (owner != GROUND_RIBBON || !FallowBarsRun(vol, *start, want, &past))
            return true;
        *barred = true;
        lo      = past;
    }
    return false;
}

error_t BlockMapTakeQuire(TagFSVolume* vol, GroundOwner owner, uint64_t blocks, uint32_t align, uint64_t keep, uint64_t first,
                          uint64_t from, uint64_t* start)
{
    TagFSLock* lock = &vol->state.lock;
    uint64_t   met  = 0;
    error_t    rc   = BlockMapDerivedStock(vol);
    if (rc != OK)
        return rc;
    TagFSLockTake(lock);
    uint64_t hi = vol->block_map_derived.map.end;
    align       = align ? align : 1u;
    if (!vol->block_map_derived.map.up) {
        rc = ERR_NOT_INITIALIZED;
    } else if (blocks == 0 || vol->block_map_derived.map.free < blocks || vol->block_map_derived.map.free - blocks < keep + AllotmentPromised(vol)) {
        rc = ERR_TAGFS_NO_SPACE;
    } else {
        /* A file's window is given back before the book is refused a run:
         * the book's ground is the book's, and a window only a hint. */
        from = from < first || from >= hi ? first : from;
        bool found = false, barred = false;
        for (uint32_t round = 0; round < 2u && !found; round++) {
            found = RunQuireFree(vol, owner, from, hi, blocks, align, start, &barred) ||
                    RunQuireFree(vol, owner, first, from + blocks < hi ? from + blocks : hi, blocks, align, start,
                                 &barred);
            if (!found && (round || !AllotmentTakeBackLocked(vol, 0)))
                break;
        }
        rc = found ? MapPut(vol, owner, 0, *start, blocks, true, &met) : barred ? ERR_WOULD_BLOCK : ERR_TAGFS_NO_SPACE;
    }
    uint64_t* loose = PagesLooseTaken(vol);
    TagFSLockDrop(lock);
    PagesLoose(loose);
    return rc;
}

// ----------------------------------------------------------------------------
// A file's data: the runs a write takes and gives back (allotment.c)
// ----------------------------------------------------------------------------

bool BlockMapDerivedRunFree(TagFSVolume* vol, uint64_t from, uint64_t want, uint64_t fid, uint64_t* start)
{
    if (!vol->block_map_derived.map.up || want == 0)
        return false;
    uint64_t lo = from < vol->block_map_derived.map.end ? from : 0;
    return RunAlignedFree(vol, lo, vol->block_map_derived.map.end, want, 1u, fid, start) ||
           (lo && RunAlignedFree(vol, 0, lo + want < vol->block_map_derived.map.end ? lo + want : vol->block_map_derived.map.end, want, 1u, fid, start));
}

bool BlockMapDerivedRunFreeBack(TagFSVolume* vol, uint64_t lo, uint64_t hi, uint64_t want, uint64_t fid, uint64_t* start)
{
    uint64_t run = 0, past = 0;
    if (!vol->block_map_derived.map.up || want == 0)
        return false;
    for (uint64_t b = hi < vol->block_map_derived.map.end ? hi : vol->block_map_derived.map.end; b > lo;) {
        uint32_t             w     = (uint32_t)((b - 1u) / BLOCK_MAP_WINDOW_BLOCKS);
        uint64_t             base  = (uint64_t)w * BLOCK_MAP_WINDOW_BLOCKS;
        uint64_t             floor = base > lo ? base : lo;
        const DerivedWindow* win   = &vol->block_map_derived.map.win[w];
        if (!win->bits && win->free == 0) {
            run = 0;
            b   = floor;
            continue;
        }
        for (; b > floor; b--) {
            uint64_t block = b - 1u, off = block - base;
            bool     free  = !win->bits || !(win->bits[off / 64u] & (1ull << (off % 64u)));
            run = free && !AllotmentOthersIn(vol, fid, block, 1, &past) ? run + 1u : 0;
            if (run == want) {
                *start = block;
                return true;
            }
        }
    }
    return false;
}

uint64_t BlockMapDerivedFreeFrom(TagFSVolume* vol, uint64_t block, uint64_t most, uint64_t fid)
{
    uint64_t n = 0, past = 0;
    while (n < most && vol->block_map_derived.map.up && BlockMapDerivedIsFree(vol, block + n) && !AllotmentOthersIn(vol, fid, block + n, 1, &past))
        n++;
    return n;
}

error_t BlockMapDerivedTakeLocked(TagFSVolume* vol, uint64_t fid, uint64_t start, uint64_t count)
{
    uint64_t met = 0;
    error_t  rc  = MapPut(vol, GROUND_FILE, fid, start, count, true, &met);
#if CONFIG_STRICT
    if (rc == OK && met)
        panic("[TagFS] DEFECT: file %llu was handed %llu block(s) the block map holds taken",
              (unsigned long long)fid, (unsigned long long)met);
#endif
    return rc;
}

error_t BlockMapDerivedGive(TagFSVolume* vol, uint64_t fid, uint64_t start, uint64_t count)
{
    return BlockMapDerivedGiveAs(vol, GROUND_FILE, fid, start, count);
}

error_t BlockMapDerivedGiveAs(TagFSVolume* vol, GroundOwner owner, uint64_t who, uint64_t start, uint64_t count)
{
    TagFSLock* lock = &vol->state.lock;
    uint64_t   met  = 0;
    error_t    rc   = BlockMapDerivedStock(vol);
    if (rc != OK)
        return rc;
    TagFSLockTake(lock);
    rc              = vol->block_map_derived.map.up ? MapPut(vol, owner, who, start, count, false, &met) : ERR_NOT_INITIALIZED;
    uint64_t* loose = PagesLooseTaken(vol);
    TagFSLockDrop(lock);
    PagesLoose(loose);
    return rc;
}

#if CONFIG_STRICT || defined(CONFIG_VOLUME_TESTS)
/* A page against its summary: its free count, its longest run, the blocks
 * past the map's end set. */
static bool PageHolds(TagFSVolume* vol, uint32_t w)
{
    const DerivedWindow* win  = &vol->block_map_derived.map.win[w];
    uint32_t             n    = WindowBlocks(vol, w), ones = 0;
    for (uint32_t i = 0; i < DERIVED_WORDS; i++)
        ones += WordOnes(win->bits[i]);
    for (uint32_t off = n; off < BLOCK_MAP_WINDOW_BLOCKS; off++)
        if (!(win->bits[off / 64u] & (1ull << (off % 64u))))
            return false;
    return BLOCK_MAP_WINDOW_BLOCKS - ones == win->free && win->free > 0 && win->free < n &&
           BitsLongest(win->bits) == win->longest;
}
#endif

// ----------------------------------------------------------------------------
// A volume that grew (mount/book_grow.c)
// ----------------------------------------------------------------------------

error_t BlockMapDerivedWidenMake(TagFSVolume* vol, uint64_t end, BlockMapWidening* w, const char** what)
{
    uint64_t windows = (end + BLOCK_MAP_WINDOW_BLOCKS - 1u) / BLOCK_MAP_WINDOW_BLOCKS;
    *w = (BlockMapWidening){ end, NULL, NULL };
    if (!vol->block_map_derived.map.up || end <= vol->block_map_derived.map.end) {
        *what = "its block map is not up, or would not be any wider";
        return ERR_INVALID_ARGUMENT;
    }
    if (windows > UINT32_MAX) {
        ksnprintf(vol->block_map_derived.words, sizeof(vol->block_map_derived.words), "its block map would count %llu windows of %u blocks, and it counts "
                  "them in 32 bits", (unsigned long long)windows, BLOCK_MAP_WINDOW_BLOCKS);
        *what = vol->block_map_derived.words;
        return ERR_OUT_OF_RANGE;
    }
    w->win  = kmalloc(sizeof(DerivedWindow) * windows);
    w->page = vol->block_map_derived.map.end % BLOCK_MAP_WINDOW_BLOCKS ? kmalloc(DERIVED_WORDS * sizeof(uint64_t)) : NULL;
    if (!w->win || (vol->block_map_derived.map.end % BLOCK_MAP_WINDOW_BLOCKS && !w->page)) {
        ksnprintf(vol->block_map_derived.words, sizeof(vol->block_map_derived.words), "there is no memory for its block map's summary of %llu windows, "
                  "%llu KiB", (unsigned long long)windows,
                  (unsigned long long)(windows * sizeof(DerivedWindow) / 1024u));
        *what = vol->block_map_derived.words;
        BlockMapDerivedWidenDrop(w);
        return ERR_NO_MEMORY;
    }
    return OK;
}

/* The map's old last window, cut short by its old end, given the blocks past
 * it up to the new end: free, the page `*page` made for it where it was whole
 * taken and is mixed now. */
static void WidenLastWindow(TagFSVolume* vol, uint32_t last, uint64_t old_end, uint64_t** page)
{
    DerivedWindow* win = &vol->block_map_derived.map.win[last];
    uint32_t       was = (uint32_t)(old_end - (uint64_t)last * BLOCK_MAP_WINDOW_BLOCKS);
    uint32_t       now = WindowBlocks(vol, last);
    if (!win->bits && win->free == 0) {
        win->bits = *page;
        *page     = NULL;
        memset(win->bits, 0xFF, DERIVED_WORDS * sizeof(uint64_t));
    }
    if (win->bits)
        BitsPut(win->bits, was, now, false);
    win->free  += now - was;
    vol->block_map_derived.map.free += now - was;
    WindowSettle(vol, last);
}

#if CONFIG_STRICT
/* The widened map held to what it was given: every window from the old last
 * one on to its page, or whole free or whole taken; every block past the old
 * end free; the windows' counts summed to the map's — a growth that handed
 * out the new ground wrongly is a DEFECT before the door opens. */
static void WidenHeld(TagFSVolume* vol, uint32_t from, uint64_t old_end)
{
    uint64_t sum = 0;
    for (uint32_t w = 0; w < vol->block_map_derived.map.windows; w++) {
        const DerivedWindow* win = &vol->block_map_derived.map.win[w];
        uint64_t             lo  = (uint64_t)w * BLOCK_MAP_WINDOW_BLOCKS, hi = lo + WindowBlocks(vol, w);
        uint64_t             b   = old_end > lo ? old_end : lo;
        bool                 held;
        sum += win->free;
        if (w < from)
            continue;
        if (!win->bits)
            held = (win->free == 0 || win->free == WindowBlocks(vol, w)) && win->longest == win->free &&
                   (b >= hi || win->free != 0);
        else
            for (held = PageHolds(vol, w); held && b < hi; b++)
                held = !(win->bits[(b - lo) / 64u] & (1ull << ((b - lo) % 64u)));
        if (!held)
            panic("[TagFS] DEFECT: the block map widened from block %llu does not hold its window %u — %u free, "
                  "longest %u", (unsigned long long)old_end, w, win->free, win->longest);
    }
    if (sum != vol->block_map_derived.map.free)
        panic("[TagFS] DEFECT: the block map widened from block %llu counts %llu free and its windows %llu",
              (unsigned long long)old_end, (unsigned long long)vol->block_map_derived.map.free, (unsigned long long)sum);
}
#endif

/*
 * ‼ THE SUMMARY IS COPIED WHOLE, AND ONLY THE GROUND PAST THE OLD END IS NEW.
 * Every window the map had keeps its count, its longest run and its page; the
 * old last window, cut short by the far Deed's block, takes the blocks past it
 * free (WidenLastWindow); every window past it is whole free. The pages a
 * window lets go of here go where any change's do (PageGiven), and the old
 * summary, the page that was not wanted and the loose ones are freed once the
 * state lock is dropped.
 */
void BlockMapDerivedWidenPut(TagFSVolume* vol, BlockMapWidening* w)
{
    TagFSLock*     lock     = &vol->state.lock;
    DerivedWindow* win      = w->win;
    uint64_t*      page     = w->page;
    uint64_t       windows  = (w->end + BLOCK_MAP_WINDOW_BLOCKS - 1u) / BLOCK_MAP_WINDOW_BLOCKS;
    TagFSLockTake(lock);
    DerivedWindow* old      = vol->block_map_derived.map.win;
    uint32_t       had      = vol->block_map_derived.map.windows;
    uint64_t       old_end  = vol->block_map_derived.map.end;
    memcpy(win, old, sizeof(DerivedWindow) * had);
    vol->block_map_derived.map.win     = win;
    vol->block_map_derived.map.end     = w->end;
    vol->block_map_derived.map.windows = (uint32_t)windows;
    for (uint32_t k = had; k < vol->block_map_derived.map.windows; k++) {
        vol->block_map_derived.map.win[k] = (DerivedWindow){ WindowBlocks(vol, k), WindowBlocks(vol, k), NULL };
        vol->block_map_derived.map.free  += WindowBlocks(vol, k);
    }
    if (old_end % BLOCK_MAP_WINDOW_BLOCKS)
        WidenLastWindow(vol, had - 1u, old_end, &page);
    vol->state.free_blocks = vol->block_map_derived.map.free;
#if CONFIG_STRICT
    WidenHeld(vol, had - 1u, old_end);
#endif
    uint64_t* loose = PagesLooseTaken(vol);
    TagFSLockDrop(lock);
    kfree(old);
    kfree(page);
    PagesLoose(loose);
    *w = (BlockMapWidening){ 0, NULL, NULL };
}

void BlockMapDerivedWidenDrop(BlockMapWidening* w)
{
    kfree(w->win);
    kfree(w->page);
    *w = (BlockMapWidening){ 0, NULL, NULL };
}

void BlockMapDerivedLetGo(TagFSVolume* vol)
{
    if (vol->block_map_derived.map.unnamed)
        kprintf("[TagFS FSCK] and %llu more claim(s) and release(s) on the block map met blocks so already\n",
                (unsigned long long)vol->block_map_derived.map.unnamed);
    for (uint32_t w = 0; vol->block_map_derived.map.win && w < vol->block_map_derived.map.windows; w++)
        kfree(vol->block_map_derived.map.win[w].bits);
    for (uint32_t i = 0; i < DERIVED_STOCK; i++)
        kfree(vol->block_map_derived.map.stock[i]);
    PagesLoose(vol->block_map_derived.map.loose);
    PagesLoose(vol->block_map_derived.map.held);
    kfree(vol->block_map_derived.map.win);
    memset(&vol->block_map_derived.map, 0, sizeof(vol->block_map_derived.map));
}

#ifdef CONFIG_VOLUME_TESTS
void BlockMapDerivedStandRefuse(bool refused)
{
    __atomic_store_n(&s_stand_refuse, refused ? 1u : 0u, __ATOMIC_RELEASE);
    if (refused)
        s_stand_short = 0;
}

uint32_t BlockMapDerivedStandShort(void)
{
    return s_stand_short;
}

/* The runs the stand holds to make the volume small, to let go of again. */
static Strip*   s_stand_held;
static uint32_t s_stand_nheld;
static uint32_t s_stand_capheld;

/* The run [start, start + count) put for the stand, its pages stocked first and
 * the state lock taken as every change of the live map takes it. */
static error_t StandPut(TagFSVolume* vol, uint64_t start, uint64_t count, bool take)
{
    uint64_t met = 0;
    error_t  rc  = BlockMapDerivedStock(vol);
    if (rc != OK)
        return rc;
    TagFSLockTake(&vol->state.lock);
    rc              = MapPut(vol, GROUND_HELD, 0, start, count, take, &met);
    uint64_t* loose = PagesLooseTaken(vol);
    TagFSLockDrop(&vol->state.lock);
    PagesLoose(loose);
    return rc;
}

/* The last run of free blocks of the map, at most `most` of them. */
static bool StandLastFree(TagFSVolume* vol, uint64_t most, uint64_t* start, uint64_t* count)
{
    uint64_t end = 0;
    for (uint64_t b = vol->block_map_derived.map.end; b-- > 0 && (!end || b + most >= end);) {
        bool free = BlockMapDerivedIsFree(vol, b);
        if (!end && free)
            end = b + 1u;
        else if (end && !free)
            break;
        *start = b;
    }
    *count = end ? end - *start : 0;
    if (*count > most) {
        *start += *count - most;
        *count  = most;
    }
    return end != 0;
}

bool BlockMapDerivedStandShrink(TagFSVolume* vol, uint64_t leave)
{
    uint64_t start = 0, count = 0;
    while (vol->block_map_derived.map.up && vol->block_map_derived.map.free > leave && StandLastFree(vol, vol->block_map_derived.map.free - leave, &start, &count)) {
        if (s_stand_nheld == s_stand_capheld) {
            uint32_t cap  = s_stand_capheld ? s_stand_capheld * 2u : 16u;
            Strip*   more = kmalloc(sizeof(Strip) * cap);
            if (!more)
                return false;
            if (s_stand_nheld)
                memcpy(more, s_stand_held, sizeof(Strip) * s_stand_nheld);
            kfree(s_stand_held);
            s_stand_held    = more;
            s_stand_capheld = cap;
        }
        if (StandPut(vol, start, count, true) != OK)
            return false;
        s_stand_held[s_stand_nheld++] = (Strip){ .start = start, .count = (uint32_t)count, .kind = STRIP_DATA };
    }
    return vol->block_map_derived.map.free == leave;
}

void BlockMapDerivedStandGrow(TagFSVolume* vol)
{
    while (s_stand_nheld) {
        Strip* r = &s_stand_held[--s_stand_nheld];
        (void)StandPut(vol, r->start, r->count, false);
    }
    kfree(s_stand_held);
    s_stand_held    = NULL;
    s_stand_capheld = 0;
}

bool BlockMapDerivedStandHolds(TagFSVolume* vol, uint64_t* free, uint32_t* mixed)
{
    bool holds = vol->block_map_derived.map.up;
    *free  = 0;
    *mixed = 0;
    for (uint32_t w = 0; vol->block_map_derived.map.win && w < vol->block_map_derived.map.windows; w++) {
        const DerivedWindow* win = &vol->block_map_derived.map.win[w];
        *free += win->free;
        if (win->bits) {
            (*mixed)++;
            holds = holds && PageHolds(vol, w);
        } else {
            holds = holds && (win->free == 0 || win->free == WindowBlocks(vol, w)) && win->longest == win->free;
        }
    }
    return holds && *free == vol->block_map_derived.map.free && *free == vol->state.free_blocks;
}
#endif
