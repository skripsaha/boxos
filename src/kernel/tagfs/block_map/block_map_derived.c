#include "block_map_derived.h"
#include "../tagfs.h"
#include "amp.h"           /* amp_get_core_index — the stand's HEAPFAIL on the builder's core */

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
#define DERIVED_STOCK  2u    /* pages one run can want: the windows at its two ends */

typedef struct {
    uint32_t  free;          /* of the window's blocks the volume has, the ones no owner names */
    uint32_t  longest;       /* the longest run of them */
    uint64_t* bits;          /* a bit a block, set where taken — a mixed window's alone */
} DerivedWindow;
_Static_assert(sizeof(DerivedWindow) == 16, "a window's summary is sixteen bytes (spec_v2 §4.6)");

static struct {
    bool           up;
    bool           building;
    bool           short_of;     /* a page refused while building: the rest is counted, not laid */
    uint64_t       end;
    uint32_t       windows;
    uint64_t       free;
    DerivedWindow* win;
    uint32_t       named;
    uint64_t       unnamed;
    uint64_t*      stock[DERIVED_STOCK];   /* pages made before the state lock, for a change of the live map */
    uint64_t*      loose;                  /* pages a change of the live map let go of, past a full stock:
                                            * chained through their first word, freed after the lock */
} s_map;

static char s_words[224];

#ifdef CONFIG_VOLUME_TESTS
static uint32_t s_stand_refuse;
static uint32_t s_stand_short;
#endif

// ----------------------------------------------------------------------------
// Pages
// ----------------------------------------------------------------------------

/* The window's blocks the volume has: all of them but in the last window. */
static uint32_t WindowBlocks(uint32_t w)
{
    uint64_t base = (uint64_t)w * BLOCK_MAP_WINDOW_BLOCKS;
    return s_map.end - base < BLOCK_MAP_WINDOW_BLOCKS ? (uint32_t)(s_map.end - base) : BLOCK_MAP_WINDOW_BLOCKS;
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
static uint64_t* PageTaken(void)
{
    if (s_map.building)
        return kmalloc(DERIVED_WORDS * sizeof(uint64_t));
    for (uint32_t i = 0; i < DERIVED_STOCK; i++) {
        uint64_t* page = s_map.stock[i];
        if (page) {
            s_map.stock[i] = NULL;
            return page;
        }
    }
    return NULL;
}

/* A page the map no longer wants: the heap's again while it is built, with no
 * lock held; once it is up — under the state lock, which nothing is given back
 * to the heap under (model A; fallow.h, block_map.c alike) — the stock's when
 * it has room, else chained loose for the change's maker to free once the
 * lock is dropped (PagesLoose). */
static void PageGiven(uint64_t* page)
{
    if (s_map.building) {
        kfree(page);
        return;
    }
    for (uint32_t i = 0; i < DERIVED_STOCK; i++) {
        if (!s_map.stock[i]) {
            s_map.stock[i] = page;
            return;
        }
    }
    page[0]     = (uint64_t)(uintptr_t)s_map.loose;
    s_map.loose = page;
}

/* The pages a change of the live map let go of, taken under the state lock. */
static uint64_t* PagesLooseTaken(void)
{
    uint64_t* loose = s_map.loose;
    s_map.loose     = NULL;
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
static uint64_t* PageMade(uint32_t w)
{
#ifdef CONFIG_VOLUME_TESTS
    if (s_map.building && __atomic_exchange_n(&s_stand_refuse, 0u, __ATOMIC_ACQ_REL))
        HeapFailArm(amp_get_core_index(), 1);
#endif
    uint64_t* bits = PageTaken();
    if (!bits)
        return NULL;
    memset(bits, s_map.win[w].free ? 0x00 : 0xFF, DERIVED_WORDS * sizeof(uint64_t));
    BitsPut(bits, WindowBlocks(w), BLOCK_MAP_WINDOW_BLOCKS, true);
    return bits;
}

/* The summary after the window changed: a window left whole lets its page go.
 * Its longest run is found again — but not while the map is built: nothing
 * asks for a run then, and a mixed window takes as many claims as it has
 * gaps, so each would look at the whole page once more. The build's end finds
 * every one once (BlockMapDerivedBuilt). */
static void WindowSettle(uint32_t w)
{
    DerivedWindow* win = &s_map.win[w];
    if (win->bits && (win->free == 0 || win->free == WindowBlocks(w))) {
        PageGiven(win->bits);
        win->bits = NULL;
    }
    if (!win->bits)
        win->longest = win->free;
    else if (!s_map.building)
        win->longest = BitsLongest(win->bits);
}

// ----------------------------------------------------------------------------
// A run put
// ----------------------------------------------------------------------------

/* Window w's part of [start, end) as [*from, *to) of the window. */
static void WindowPart(uint32_t w, uint64_t start, uint64_t end, uint32_t* from, uint32_t* to)
{
    uint64_t base = (uint64_t)w * BLOCK_MAP_WINDOW_BLOCKS;
    uint32_t n    = WindowBlocks(w);
    *from = start > base ? (uint32_t)(start - base) : 0;
    *to   = end - base < n ? (uint32_t)(end - base) : n;
}

/* Whether the run leaves window w mixed and it has no page to say so: a part
 * of it put to the state the whole window is not in. */
static bool WindowWantsPage(uint32_t w, uint64_t start, uint64_t end, bool take)
{
    const DerivedWindow* win = &s_map.win[w];
    uint32_t             n   = WindowBlocks(w), from, to;
    WindowPart(w, start, end, &from, &to);
    return !win->bits && (from > 0 || to < n) && (take ? win->free == n : win->free == 0);
}

/* The run's part in window w put: how many of its blocks were so already. A
 * window with no page whose part is in the state asked already is left as it
 * is; one with none while the build is short of pages is counted alone. */
static uint32_t WindowPut(uint32_t w, uint64_t start, uint64_t end, bool take)
{
    DerivedWindow* win = &s_map.win[w];
    uint32_t       n   = WindowBlocks(w), was = win->free, from, to, met = 0;
    WindowPart(w, start, end, &from, &to);
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
    s_map.free = s_map.free - was + win->free;
    WindowSettle(w);
    return met;
}

/* [start, end) put to `take`: the pages its two edges need made first — a
 * run's inner windows it covers whole — so that nothing changes unless all of
 * it can; *met how many were so already. ERR_NO_MEMORY, nothing changed, for a
 * page refused once the map is up; while it is built, the build counts on. */
static error_t RunPut(uint64_t start, uint64_t end, bool take, uint64_t* met)
{
    uint32_t w0 = (uint32_t)(start / BLOCK_MAP_WINDOW_BLOCKS);
    uint32_t w1 = (uint32_t)((end - 1u) / BLOCK_MAP_WINDOW_BLOCKS);
    bool     made0 = false;

    for (uint32_t k = 0; k < 2u && !s_map.short_of; k++) {
        uint32_t w = k ? w1 : w0;
        if ((k && w1 == w0) || !WindowWantsPage(w, start, end, take))
            continue;
        if ((s_map.win[w].bits = PageMade(w)) != NULL) {
            made0 = made0 || k == 0;
            continue;
        }
        if (made0) {
            PageGiven(s_map.win[w0].bits);
            s_map.win[w0].bits = NULL;
        }
        if (!s_map.building)
            return ERR_NO_MEMORY;
        s_map.short_of = true;
    }
    for (uint32_t w = w0; w <= w1; w++)
        *met += WindowPut(w, start, end, take);
    return OK;
}

/* A claim that met blocks taken already, or a release that met blocks free:
 * the first DERIVED_NAMED named, the rest counted — and the first of the rest
 * says that they are, so a machine halted before the map is let go has said
 * there were more. */
static void MetSay(GroundOwner owner, uint64_t who, uint64_t start, uint64_t count, uint64_t met, bool take)
{
    char by[48];
    if (s_map.named >= DERIVED_NAMED) {
        if (s_map.unnamed++ == 0)
            kprintf("[TagFS FSCK] more claims and releases on the block map meet blocks so already than are "
                    "named one by one (%u) — from here they are counted, and the count said when the map is "
                    "let go\n", DERIVED_NAMED);
        return;
    }
    s_map.named++;
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

static error_t MapPut(GroundOwner owner, uint64_t who, uint64_t start, uint64_t count, bool take, uint64_t* met)
{
    *met = 0;
    if (!s_map.win)
        return ERR_NOT_INITIALIZED;
    uint64_t end = count > s_map.end - (start < s_map.end ? start : s_map.end) ? s_map.end : start + count;
    if (start >= end)
        return OK;
    error_t rc = RunPut(start, end, take, met);
    if (rc == OK && *met)
        MetSay(owner, who, start, end - start, *met, take);
    if (s_map.up)
        tagfs_get_state()->ledger.free_blocks = s_map.free;
    return rc;
}

error_t BlockMapDerivedClaim(GroundOwner owner, uint64_t who, uint64_t start, uint64_t count, uint64_t* taken)
{
    return MapPut(owner, who, start, count, true, taken);
}

error_t BlockMapDerivedRelease(GroundOwner owner, uint64_t who, uint64_t start, uint64_t count, uint64_t* free)
{
    return MapPut(owner, who, start, count, false, free);
}

// ----------------------------------------------------------------------------
// The build, the questions, letting go
// ----------------------------------------------------------------------------

error_t BlockMapDerivedBegin(uint64_t end, const char** what)
{
    BlockMapDerivedLetGo();
    uint64_t windows = (end + BLOCK_MAP_WINDOW_BLOCKS - 1u) / BLOCK_MAP_WINDOW_BLOCKS;
    if (windows > UINT32_MAX) {
        ksnprintf(s_words, sizeof(s_words), "its block map would count %llu windows of %u blocks, and it counts "
                  "them in 32 bits", (unsigned long long)windows, BLOCK_MAP_WINDOW_BLOCKS);
        *what = s_words;
        return ERR_OUT_OF_RANGE;
    }
    s_map.win = kmalloc(sizeof(DerivedWindow) * windows);
    if (!s_map.win) {
        ksnprintf(s_words, sizeof(s_words), "there is no memory for its block map's summary: %llu windows, "
                  "%llu KiB", (unsigned long long)windows,
                  (unsigned long long)(windows * sizeof(DerivedWindow) / 1024u));
        *what = s_words;
        return ERR_NO_MEMORY;
    }
    s_map.end      = end;
    s_map.windows  = (uint32_t)windows;
    s_map.free     = end;
    s_map.building = true;
    for (uint32_t w = 0; w < s_map.windows; w++)
        s_map.win[w] = (DerivedWindow){ WindowBlocks(w), WindowBlocks(w), NULL };
    return OK;
}

/* Windows both free and taken. */
static uint32_t MapMixed(void)
{
    uint32_t mixed = 0;
    for (uint32_t w = 0; w < s_map.windows; w++)
        mixed += s_map.win[w].free > 0 && s_map.win[w].free < WindowBlocks(w);
    return mixed;
}

error_t BlockMapDerivedBuilt(const char** what)
{
    s_map.building = false;
    if (s_map.short_of) {
        uint32_t mixed = MapMixed();
        ksnprintf(s_words, sizeof(s_words), "its block map is short of memory: %u mixed window(s) want %llu "
                  "bytes of pages past the %llu bytes of its summary", mixed,
                  (unsigned long long)mixed * (DERIVED_WORDS * sizeof(uint64_t)),
                  (unsigned long long)s_map.windows * sizeof(DerivedWindow));
#ifdef CONFIG_VOLUME_TESTS
        s_stand_short = mixed;
#endif
        BlockMapDerivedLetGo();
        *what = s_words;
        return ERR_NO_MEMORY;
    }
    for (uint32_t w = 0; w < s_map.windows; w++)
        if (s_map.win[w].bits)
            s_map.win[w].longest = BitsLongest(s_map.win[w].bits);
    s_map.up = true;
    tagfs_get_state()->ledger.free_blocks = s_map.free;
    return OK;
}

bool BlockMapDerivedUp(void)
{
    return s_map.up;
}

bool BlockMapDerivedIsFree(uint64_t block)
{
    if (!s_map.win || block >= s_map.end)
        return false;
    const DerivedWindow* win = &s_map.win[block / BLOCK_MAP_WINDOW_BLOCKS];
    uint32_t             off = (uint32_t)(block % BLOCK_MAP_WINDOW_BLOCKS);
    return win->bits ? !(win->bits[off / 64u] & (1ull << (off % 64u))) : win->free != 0;
}

bool BlockMapDerivedRunIn(uint32_t window, uint32_t want, uint64_t* start)
{
    if (!s_map.win || window >= s_map.windows || want == 0 || s_map.win[window].longest < want)
        return false;
    const DerivedWindow* win  = &s_map.win[window];
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

error_t BlockMapDerivedStock(void)
{
    TagFSLock* lock = &tagfs_get_state()->lock;
    for (uint32_t i = 0; i < DERIVED_STOCK; i++) {
        TagFSLockTake(lock);
        bool full = s_map.stock[i] != NULL;
        TagFSLockDrop(lock);
        if (full)
            continue;
        uint64_t* page = kmalloc(DERIVED_WORDS * sizeof(uint64_t));
        if (!page)
            return ERR_NO_MEMORY;
        TagFSLockTake(lock);
        if (!s_map.stock[i]) {
            s_map.stock[i] = page;
            page           = NULL;
        }
        TagFSLockDrop(lock);
        kfree(page);
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
 * *start. A window whole free or whole taken is passed by its summary. */
static bool RunAlignedIn(uint64_t lo, uint64_t hi, uint64_t want, uint32_t align, uint64_t* start)
{
    uint64_t run = 0, at = 0;
    for (uint64_t b = lo; b < hi;) {
        uint32_t             w    = (uint32_t)(b / BLOCK_MAP_WINDOW_BLOCKS);
        uint64_t             base = (uint64_t)w * BLOCK_MAP_WINDOW_BLOCKS;
        uint64_t             end  = base + WindowBlocks(w) < hi ? base + WindowBlocks(w) : hi;
        const DerivedWindow* win  = &s_map.win[w];
        if (!win->bits) {
            if (win->free == 0)
                run = 0;
            else if (RunGrows(b, end - b, align, want, &at, &run))
                break;
            b = end;
            continue;
        }
        for (; b < end; b++) {
            uint32_t off = (uint32_t)(b - base);
            if (win->bits[off / 64u] & (1ull << (off % 64u)))
                run = 0;
            else if (RunGrows(b, 1, align, want, &at, &run))
                break;
        }
        if (run >= want)
            break;
    }
    if (run < want || at + want > hi)
        return false;
    *start = at;
    return true;
}

error_t BlockMapTakeQuire(uint64_t blocks, uint32_t align, uint64_t keep, uint64_t first, uint64_t from,
                          uint64_t* start)
{
    TagFSLock* lock = &tagfs_get_state()->lock;
    uint64_t   met  = 0;
    error_t    rc   = BlockMapDerivedStock();
    if (rc != OK)
        return rc;
    TagFSLockTake(lock);
    uint64_t hi = s_map.end;
    if (!s_map.up) {
        rc = ERR_NOT_INITIALIZED;
    } else if (blocks == 0 || s_map.free < blocks || s_map.free - blocks < keep) {
        rc = ERR_TAGFS_NO_SPACE;
    } else {
        from = from < first || from >= hi ? first : from;
        if (!RunAlignedIn(from, hi, blocks, align ? align : 1u, start) &&
            !RunAlignedIn(first, from + blocks < hi ? from + blocks : hi, blocks, align ? align : 1u, start))
            rc = ERR_TAGFS_NO_SPACE;
        else
            rc = MapPut(GROUND_RIBBON, 0, *start, blocks, true, &met);
    }
    uint64_t* loose = PagesLooseTaken();
    TagFSLockDrop(lock);
    PagesLoose(loose);
    return rc;
}

void BlockMapDerivedLetGo(void)
{
    if (s_map.unnamed)
        kprintf("[TagFS FSCK] and %llu more claim(s) and release(s) on the block map met blocks so already\n",
                (unsigned long long)s_map.unnamed);
    for (uint32_t w = 0; s_map.win && w < s_map.windows; w++)
        kfree(s_map.win[w].bits);
    for (uint32_t i = 0; i < DERIVED_STOCK; i++)
        kfree(s_map.stock[i]);
    PagesLoose(s_map.loose);
    kfree(s_map.win);
    memset(&s_map, 0, sizeof(s_map));
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

/* A page against its summary: its free count, its longest run, the blocks
 * past the map's end set. */
static bool PageHolds(uint32_t w)
{
    const DerivedWindow* win  = &s_map.win[w];
    uint32_t             n    = WindowBlocks(w), ones = 0;
    for (uint32_t i = 0; i < DERIVED_WORDS; i++)
        ones += WordOnes(win->bits[i]);
    for (uint32_t off = n; off < BLOCK_MAP_WINDOW_BLOCKS; off++)
        if (!(win->bits[off / 64u] & (1ull << (off % 64u))))
            return false;
    return BLOCK_MAP_WINDOW_BLOCKS - ones == win->free && win->free > 0 && win->free < n &&
           BitsLongest(win->bits) == win->longest;
}

/* The runs the stand holds to make the volume small, to let go of again. */
static Strip*   s_stand_held;
static uint32_t s_stand_nheld;
static uint32_t s_stand_capheld;

/* The run [start, start + count) put for the stand, its pages stocked first and
 * the state lock taken as every change of the live map takes it. */
static error_t StandPut(uint64_t start, uint64_t count, bool take)
{
    uint64_t met = 0;
    error_t  rc  = BlockMapDerivedStock();
    if (rc != OK)
        return rc;
    TagFSLockTake(&tagfs_get_state()->lock);
    rc              = MapPut(GROUND_HELD, 0, start, count, take, &met);
    uint64_t* loose = PagesLooseTaken();
    TagFSLockDrop(&tagfs_get_state()->lock);
    PagesLoose(loose);
    return rc;
}

/* The last run of free blocks of the map, at most `most` of them. */
static bool StandLastFree(uint64_t most, uint64_t* start, uint64_t* count)
{
    uint64_t end = 0;
    for (uint64_t b = s_map.end; b-- > 0 && (!end || b + most >= end);) {
        bool free = BlockMapDerivedIsFree(b);
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

bool BlockMapDerivedStandShrink(uint64_t leave)
{
    uint64_t start = 0, count = 0;
    while (s_map.up && s_map.free > leave && StandLastFree(s_map.free - leave, &start, &count)) {
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
        if (StandPut(start, count, true) != OK)
            return false;
        s_stand_held[s_stand_nheld++] = (Strip){ .start = start, .count = (uint32_t)count, .kind = STRIP_DATA };
    }
    return s_map.free == leave;
}

void BlockMapDerivedStandGrow(void)
{
    while (s_stand_nheld) {
        Strip* r = &s_stand_held[--s_stand_nheld];
        (void)StandPut(r->start, r->count, false);
    }
    kfree(s_stand_held);
    s_stand_held    = NULL;
    s_stand_capheld = 0;
}

bool BlockMapDerivedStandHolds(uint64_t* free, uint32_t* mixed)
{
    bool holds = s_map.up;
    *free  = 0;
    *mixed = 0;
    for (uint32_t w = 0; s_map.win && w < s_map.windows; w++) {
        const DerivedWindow* win = &s_map.win[w];
        *free += win->free;
        if (win->bits) {
            (*mixed)++;
            holds = holds && PageHolds(w);
        } else {
            holds = holds && (win->free == 0 || win->free == WindowBlocks(w)) && win->longest == win->free;
        }
    }
    return holds && *free == s_map.free && *free == tagfs_get_state()->ledger.free_blocks;
}
#endif
