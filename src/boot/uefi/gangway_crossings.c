/*
 * Gangway — the crossings (boxos_crossings.h): every start this loader hands a
 * kernel over, kept in the crossings file of the ESP on the volume's medium,
 * and whether the kernel marked it landed.
 *
 * Read once, before the panel is decided on (CrossingsTake): the crossings
 * that check out, newest first, the number the next carries, the slot after
 * the newest, and how many in a row did not land — CROSSINGS_MISSES_PANEL of
 * them open the panel by themselves. This start's is written once the panel,
 * if any, is left (CrossingKeep): a start the person restarts or turns off
 * there never crossed, and is no miss. The kernel is told where it lies and
 * its number by a Boarding Pass stamp (CrossingStamp, gangway_handoff.c).
 *
 * Nothing is written to a medium with no ESP carrying the file in one run
 * (MediumCrossings), or whose file carries no head a BoxOS tool laid, or that
 * refuses writes: each is said, and the start goes on with no crossing kept.
 */

#include "gangway.h"

_Static_assert(GANGWAY_SET_SCREEN == CROSSING_ASKED_SCREEN && GANGWAY_SET_STARTUP == CROSSING_ASKED_STARTUP &&
                   GANGWAY_SET_CORES == CROSSING_ASKED_CORES && GANGWAY_SET_KEEP_LOG == CROSSING_ASKED_KEEP_LOG &&
                   GANGWAY_SET_SCENE == CROSSING_ASKED_SCENE,
               "a crossing keeps what was asked by the bits Gangway keeps it by");

/* The head and every slot, read at once. */
#define FRONT_SECTORS (CROSSINGS_FILE_BYTES / 512u)
#define FRONT_PAGES   (CROSSINGS_FILE_BYTES / 4096u)

typedef struct {
    const char *none;                    /* why no crossings are kept here, or NULL */
    Crossing    kept[CROSSINGS_SLOTS];   /* those that check out, newest first */
    uint32_t    count;
    uint32_t    slot_next;               /* the slot this start's goes into */
    uint64_t    number_next;
    uint32_t    misses;                  /* in a row from the newest, not landed */
    uint64_t    base;                    /* the file's first sector of the medium */
    uint64_t    sector;                  /* where this start's was written — */
    int         written;                 /* — when it was */
} Crossings;

static Crossings g_crossings;

static int HeadSound(const CrossingsHead *h)
{
    CrossingsHead c;
    MemCopy(&c, h, sizeof(c));
    uint32_t crc = c.crc;
    c.crc        = 0;
    return MemEqual(c.magic, CROSSINGS_HEAD_MAGIC, sizeof(c.magic)) && c.size == sizeof(c) &&
           Crc32((const uint8_t *)&c, sizeof(c)) == crc && c.slots == CROSSINGS_SLOTS &&
           c.slot_sectors == CROSSINGS_SLOT_SECTORS && c.first_slot == CROSSINGS_FIRST_SLOT;
}

static int CrossingSound(const Crossing *x)
{
    Crossing c;
    MemCopy(&c, x, sizeof(c));
    uint32_t crc = c.crc;
    c.crc        = 0;
    return MemEqual(c.magic, CROSSING_MAGIC, sizeof(c.magic)) && c.size == sizeof(c) && c.number &&
           Crc32((const uint8_t *)&c, sizeof(c)) == crc;
}

/* Into the list, newest first. */
static void Kept(Crossings *g, const Crossing *x)
{
    uint32_t at = g->count;
    while (at > 0 && g->kept[at - 1u].number < x->number) {
        MemCopy(&g->kept[at], &g->kept[at - 1u], sizeof(*x));
        at--;
    }
    MemCopy(&g->kept[at], x, sizeof(*x));
    g->count++;
}

/* Every slot of the front read: what checks out kept, the newest found, and
 * the misses counted back from it while the numbers run on unbroken. */
static void Read(Crossings *g, const uint8_t *front)
{
    uint64_t newest      = 0;
    uint32_t newest_slot = CROSSINGS_SLOTS - 1u;

    for (uint32_t s = 0; s < CROSSINGS_SLOTS; s++) {
        const Crossing *x = (const Crossing *)(front + (1u + s) * CROSSINGS_SLOT_BYTES);
        if (!CrossingSound(x)) continue;
        Kept(g, x);
        if (x->number > newest) {
            newest      = x->number;
            newest_slot = s;
        }
    }
    g->slot_next   = (newest_slot + 1u) % CROSSINGS_SLOTS;
    g->number_next = newest + 1u;
    while (g->misses < g->count && g->kept[g->misses].number == newest - g->misses &&
           g->kept[g->misses].landed != CROSSING_LANDED)
        g->misses++;
}

static void Said(const Crossings *g)
{
    if (g->none) {
        Print("Gangway: no crossings are kept on this medium — ");
        Print(g->none);
        Print("\n");
        return;
    }
    Print("Gangway: crossings: ");
    PrintDec(g->count);
    Print(" kept on this medium");
    if (g->count) {
        const Crossing *newest = &g->kept[0];
        Print(", the newest crossing ");
        PrintDec(newest->number);
        if (newest->landed == CROSSING_LANDED) {
            Print(" landed ");
            PrintDec(newest->landed_ms);
            Print(" ms after its kernel began");
        } else {
            Print(" did not land");
        }
    }
    Print("\n");
    if (g->misses >= CROSSINGS_MISSES_PANEL) {
        Print("Gangway: the last ");
        PrintDec(g->misses);
        Print(" starts did not land — the panel opens by itself\n");
    }
}

void CrossingsTake(void)
{
    Crossings           *g  = &g_crossings;
    EFI_PHYSICAL_ADDRESS at = 0;

    MemZero(g, sizeof(*g));
    g->number_next = 1;
    if ((g->none = MediumCrossings(&g->base)) != NULL) {
        /* said below */
    } else if (EFI_ERROR(g_bs->allocate_pages(AllocateAnyPages, EfiLoaderData, FRONT_PAGES, &at))) {
        g->none = "the loader has no room to read them";
    } else {
        const uint8_t *front = (const uint8_t *)(uintptr_t)at;
        if (EFI_ERROR(MediumRead(g->base, FRONT_SECTORS, (void *)front)))
            g->none = "the medium would not read the crossings file";
        else if (!HeadSound((const CrossingsHead *)front))
            g->none = "the crossings file carries no head — no BoxOS tool laid it";
        else
            Read(g, front);
        g_bs->free_pages(at, FRONT_PAGES);
    }
    Said(g);
}

uint32_t CrossingsMisses(void)
{
    return g_crossings.none ? 0u : g_crossings.misses;
}

const char *CrossingsNone(void)
{
    return g_crossings.none;
}

uint64_t CrossingsNext(void)
{
    return g_crossings.number_next;
}

uint32_t CrossingsKept(const Crossing **list)
{
    *list = g_crossings.kept;
    return g_crossings.none ? 0u : g_crossings.count;
}

void CrossingKeep(uint8_t panel, const FbInfo *fb)
{
    static uint8_t slot[CROSSINGS_SLOT_BYTES];
    Crossings     *g = &g_crossings;
    Crossing      *x = (Crossing *)slot;
    EFI_TIME       now;

    if (g->none) return;
    MemZero(slot, sizeof(slot));
    MemCopy(x->magic, CROSSING_MAGIC, sizeof(x->magic));
    x->size   = sizeof(*x);
    x->number = g->number_next;
    MemCopy(x->volume, g_volume_uuid, sizeof(x->volume));
    if (!EFI_ERROR(g_st->runtime_services->get_time(&now, NULL))) {
        x->year   = now.year;
        x->month  = now.month;
        x->day    = now.day;
        x->hour   = now.hour;
        x->minute = now.minute;
        x->second = now.second;
        x->clock  = 1;
    }
    x->kernel        = KernelChosen()->says == ATTEST_HELD ? CROSSING_KERNEL_HELD : CROSSING_KERNEL_UNCHECKED;
    x->panel         = panel;
    x->startup       = g_settings.startup;
    x->keep_log      = g_settings.keep_log;
    x->scene         = g_settings.scene;
    x->asked         = g_settings.set;
    x->cores         = g_settings.cores;
    x->screen_width  = fb->width;
    x->screen_height = fb->height;
    x->misses        = g->misses;
    x->crc           = Crc32(slot, sizeof(*x));

    uint64_t   sector = g->base + CROSSINGS_FIRST_SLOT + (uint64_t)g->slot_next * CROSSINGS_SLOT_SECTORS;
    EFI_STATUS status = MediumWriteSlot(sector, slot);
    if (EFI_ERROR(status)) {
        Print("Gangway: crossing ");
        PrintDec(x->number);
        Print(status == EFI_WRITE_PROTECTED ? " is not kept — the medium refuses writes\n"
                                            : " is not kept — the medium would not write its slot, status=");
        if (status != EFI_WRITE_PROTECTED) {
            PrintHex64((uint64_t)status);
            Print("\n");
        }
        return;
    }
    g->sector  = sector;
    g->written = 1;
    Print("Gangway: this start is crossing ");
    PrintDec(x->number);
    Print(", kept at sector ");
    PrintDec(sector);
    Print("\n");
    Moment("crossing");
}

int CrossingStamp(BoardingCrossing *out)
{
    if (!g_crossings.written) return 0;
    out->sector = g_crossings.sector;
    out->number = g_crossings.number_next;
    return 1;
}
