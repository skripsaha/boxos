/*
 * Gangway — the panel. Asked for by holding Space while the machine starts
 * (KeyPanelAsked, once, after the kernel and the screen are chosen), or open
 * by itself when the last starts did not land (the crossings,
 * gangway_crossings.c), it shows what the loader found and chose — the kernel
 * and the book's word on it, the volume, the screen, the processors, the
 * crossings — and lets the person change, for this one start, what
 * gangway.settings sets: the same five settings, taken as the file's are.
 * It shows this start's journal, the log the kernel kept of the start before
 * and the crossings kept (gangway_panel_log.c), and it restarts the machine,
 * turns it off, or restarts it into the firmware's own settings.
 *
 * Nothing in it waits on a clock: the firmware's watchdog is stopped when it
 * opens, keys are waited for on the console's event, and it stays until the
 * person chooses. It writes nothing to the volume — a choice kept is set in
 * the hub (`settings gangway`), and the page says so. What it says goes into
 * the journal and down COM1 both (PrintAloud): a machine standing at the panel
 * says why on the wire.
 */

#include "gangway.h"
#include "boarding_pass.h"
#include "video_colors.h"

/* The settings' rows first, in the order of the declaration
 * (boxos_gangway_settings.h), then what may be done. */
typedef enum {
    ROW_SCREEN,
    ROW_CORES,
    ROW_STARTUP,
    ROW_KEEP_LOG,
    ROW_SCENE,
    ROW_START,
    ROW_JOURNAL,
    ROW_PREVIOUS,
    ROW_CROSSINGS,
    ROW_RESTART,
    ROW_OFF,
    ROW_FIRMWARE,
} PanelRow;

#define PANEL_SETTINGS    ROW_START
#define PANEL_SIZES_MOST  48u
/* The most cores the declaration takes (cores number from 1 to 256). */
#define PANEL_CORES_MOST  256u

#define ATTR_TITLE    VIDEO_ATTR_INPUT
#define ATTR_NAME     VIDEO_ATTR_HINT
#define ATTR_CHOSEN   VIDEO_WHITE_ON_BLUE

typedef struct {
    uint8_t     why;                      /* CROSSING_PANEL_KEY or _MISSES */
    uint32_t    misses;                   /* starts in a row that did not land */
    FbInfo      fb;                       /* the screen as the journey chose it */
    GopSize     sizes[PANEL_SIZES_MOST];
    uint32_t    nsizes;
    uint32_t    machine_cores;            /* as the firmware counts them, 0 when it does not say */
    int         firmware_ui;              /* the firmware can be asked to start its settings */
    /* Each setting's value — screen: 0 auto, else sizes[v - 1]; cores: 0 all,
     * else so many; startup: BOARDING_STARTUP_*; keep-log, scene: 1 on — and
     * as the panel found it. */
    uint32_t    value[PANEL_SETTINGS];
    uint32_t    came[PANEL_SETTINGS];
    PanelRow    row;
    const char *note;                     /* what the last thing done could not do */
} Panel;

static CHAR16 g_os_indications[]           = u"OsIndications";
static CHAR16 g_os_indications_supported[] = u"OsIndicationsSupported";

static const char *const g_row_names[] = {
    "Screen", "Cores", "Startup", "Keep-log", "Scene",
    "Start", "This start's journal", "The previous start's log", "The crossings", "Restart", "Turn off",
    "Firmware settings",
};

/* ------------------------------------------------------------------------- */
/* A page                                                                    */
/* ------------------------------------------------------------------------- */

void PanelBegin(PanelPage *page)
{
    page->used  = 0;
    page->count = 0;
}

/* A page bigger than its room is cut: none of the panel's comes near it. */
void PanelPut(PanelPage *page, uint8_t attr, const char *text)
{
    for (; *text && page->used < PANEL_TEXT_BYTES; text++) {
        DrawPiece *last = page->count ? &page->pieces[page->count - 1u] : NULL;
        if (!last || last->attr != attr || last->text + last->bytes != page->text + page->used) {
            if (page->count == PANEL_PIECES_MOST) return;
            last        = &page->pieces[page->count++];
            last->text  = page->text + page->used;
            last->bytes = 0;
            last->attr  = attr;
        }
        page->text[page->used++] = *text;
        last->bytes++;
    }
}

void PanelNum(PanelPage *page, uint8_t attr, uint64_t n)
{
    char digits[21];
    int  at = 20;
    digits[at] = '\0';
    do {
        digits[--at] = (char)('0' + n % 10u);
        n /= 10u;
    } while (n);
    PanelPut(page, attr, digits + at);
}

void PanelRaw(PanelPage *page, uint8_t attr, const char *text, uint32_t bytes)
{
    if (!bytes || page->count == PANEL_PIECES_MOST) return;
    page->pieces[page->count].text  = text;
    page->pieces[page->count].bytes = bytes;
    page->pieces[page->count].attr  = attr;
    page->count++;
}

/* "2.0 GiB", "512 MiB". */
static void PanelBytes(PanelPage *page, uint8_t attr, uint64_t bytes)
{
    const uint64_t mib = 1024u * 1024u, gib = 1024u * mib;
    if (bytes < gib) {
        PanelNum(page, attr, bytes / mib);
        PanelPut(page, attr, " MiB");
        return;
    }
    PanelNum(page, attr, bytes / gib);
    PanelPut(page, attr, ".");
    PanelNum(page, attr, bytes % gib * 10u / gib);
    PanelPut(page, attr, " GiB");
}

/* ------------------------------------------------------------------------- */
/* What the machine offers                                                   */
/* ------------------------------------------------------------------------- */

static uint32_t MachineCores(void)
{
    EFI_GUID                  guid = EFI_MP_SERVICES_PROTOCOL_GUID;
    EFI_MP_SERVICES_PROTOCOL *mp   = NULL;
    UINTN                     all = 0, enabled = 0;
    if (EFI_ERROR(g_bs->locate_protocol(&guid, NULL, (void **)&mp)) || !mp ||
        EFI_ERROR(mp->get_number_of_processors(mp, &all, &enabled)) || !enabled)
        return 0;
    return enabled > PANEL_CORES_MOST ? PANEL_CORES_MOST : (uint32_t)enabled;
}

static int FirmwareUiOffered(void)
{
    EFI_GUID guid  = EFI_GLOBAL_VARIABLE_GUID;
    uint64_t value = 0;
    UINTN    bytes = sizeof(value);
    uint32_t attributes;
    return !EFI_ERROR(g_st->runtime_services->get_variable(g_os_indications_supported, &guid, &attributes, &bytes,
                                                           &value)) &&
           bytes == sizeof(value) && (value & EFI_OS_INDICATIONS_BOOT_TO_FW_UI);
}

static void PanelTake(Panel *p, const FbInfo *fb, uint8_t why, uint32_t misses)
{
    MemZero(p, sizeof(*p));
    p->why    = why;
    p->misses = misses;
    MemCopy(&p->fb, fb, sizeof(*fb));
    p->nsizes        = GopSizes(p->sizes, PANEL_SIZES_MOST);
    p->machine_cores = MachineCores();
    p->firmware_ui   = FirmwareUiOffered();
    if ((g_settings.set & GANGWAY_SET_SCREEN) && g_settings.screen_width) {
        for (uint32_t i = 0; i < p->nsizes; i++)
            if (p->sizes[i].width == g_settings.screen_width && p->sizes[i].height == g_settings.screen_height)
                p->came[ROW_SCREEN] = i + 1u;
    }
    p->came[ROW_CORES]    = g_settings.cores;
    p->came[ROW_STARTUP]  = g_settings.startup;
    p->came[ROW_KEEP_LOG] = g_settings.keep_log;
    p->came[ROW_SCENE]    = g_settings.scene;
    MemCopy(p->value, p->came, sizeof(p->value));
    p->row = ROW_START;
}

static PanelRow LastRow(const Panel *p)
{
    return p->firmware_ui ? ROW_FIRMWARE : ROW_OFF;
}

/* ------------------------------------------------------------------------- */
/* The page                                                                  */
/* ------------------------------------------------------------------------- */

static void FoundKernel(PanelPage *page)
{
    const LecternKernel *k = KernelChosen();

    PanelPut(page, ATTR_NAME, " Kernel      ");
    PanelPut(page, VIDEO_ATTR_DEFAULT, k->line);
    PanelPut(page, VIDEO_ATTR_DEFAULT, ", ");
    PanelNum(page, VIDEO_ATTR_DEFAULT, k->bytes);
    PanelPut(page, VIDEO_ATTR_DEFAULT, " bytes — ");
    if (k->says == ATTEST_HELD) PanelPut(page, VIDEO_ATTR_SUCCESS, "attested by the book\n");
    else PanelPut(page, VIDEO_ATTR_WARNING, "not attested: nothing to hold it to, and nothing shows it broken\n");
}

/* The volume's name as volume-read shows it — the first eight bytes of its
 * Deed's uuid — and its size. */
static void FoundVolume(PanelPage *page)
{
    static const char hex[] = "0123456789abcdef";

    PanelPut(page, ATTR_NAME, " Volume      ");
    for (uint32_t i = 0; i < 8u; i++) {
        char pair[3] = { hex[g_volume_uuid[i] >> 4], hex[g_volume_uuid[i] & 15u], '\0' };
        PanelPut(page, VIDEO_ATTR_DEFAULT, pair);
    }
    PanelPut(page, VIDEO_ATTR_DEFAULT, "…, ");
    PanelBytes(page, VIDEO_ATTR_DEFAULT, g_volume_end * VOLUME_BLOCK_BYTES);
    PanelPut(page, VIDEO_ATTR_DEFAULT, "\n");
}

static void FoundMachine(PanelPage *page, const Panel *p)
{
    PanelPut(page, ATTR_NAME, " Screen      ");
    PanelNum(page, VIDEO_ATTR_DEFAULT, p->fb.width);
    PanelPut(page, VIDEO_ATTR_DEFAULT, "x");
    PanelNum(page, VIDEO_ATTR_DEFAULT, p->fb.height);
    PanelPut(page, VIDEO_ATTR_DEFAULT, " — ");
    PanelPut(page, VIDEO_ATTR_DEFAULT, GopChosenWhy());
    PanelPut(page, VIDEO_ATTR_DEFAULT, "\n");

    PanelPut(page, ATTR_NAME, " Processors  ");
    if (p->machine_cores) {
        PanelNum(page, VIDEO_ATTR_DEFAULT, p->machine_cores);
        PanelPut(page, VIDEO_ATTR_DEFAULT, ", as the firmware counts them\n");
    } else {
        PanelPut(page, VIDEO_ATTR_DEFAULT, "the firmware does not say how many\n");
    }
}

/* Which crossing this start is, and how the one before it went — or why the
 * medium keeps none. */
static void FoundCrossings(PanelPage *page, const Panel *p)
{
    const Crossing *kept;
    uint32_t        count = CrossingsKept(&kept);

    PanelPut(page, ATTR_NAME, " Crossings   ");
    if (CrossingsNone()) {
        PanelPut(page, VIDEO_ATTR_DEFAULT, "none kept: ");
        PanelPut(page, VIDEO_ATTR_DEFAULT, CrossingsNone());
        PanelPut(page, VIDEO_ATTR_DEFAULT, "\n");
        return;
    }
    PanelPut(page, VIDEO_ATTR_DEFAULT, "this start is crossing ");
    PanelNum(page, VIDEO_ATTR_DEFAULT, CrossingsNext());
    if (!count) {
        PanelPut(page, VIDEO_ATTR_DEFAULT, ", the first on this medium\n");
    } else if (p->misses) {
        PanelPut(page, VIDEO_ATTR_DEFAULT, " — ");
        PanelPut(page, VIDEO_ATTR_WARNING, p->misses == 1u ? "the one before did not land" : "the last ");
        if (p->misses > 1u) {
            PanelNum(page, VIDEO_ATTR_WARNING, p->misses);
            PanelPut(page, VIDEO_ATTR_WARNING, " did not land");
        }
        PanelPut(page, VIDEO_ATTR_DEFAULT, "\n");
    } else {
        PanelPut(page, VIDEO_ATTR_DEFAULT, " — the one before landed ");
        PanelNum(page, VIDEO_ATTR_DEFAULT, kept[0].landed_ms);
        PanelPut(page, VIDEO_ATTR_DEFAULT, " ms after its kernel began\n");
    }
}

static void ValueSaid(PanelPage *page, const Panel *p, PanelRow row, uint8_t attr)
{
    uint32_t v = p->value[row];
    switch (row) {
    case ROW_SCREEN:
        if (!v) {
            PanelPut(page, attr, "auto");
            return;
        }
        PanelNum(page, attr, p->sizes[v - 1u].width);
        PanelPut(page, attr, "x");
        PanelNum(page, attr, p->sizes[v - 1u].height);
        return;
    case ROW_CORES:
        if (v) PanelNum(page, attr, v);
        else PanelPut(page, attr, "all");
        return;
    case ROW_STARTUP:  PanelPut(page, attr, v == BOARDING_STARTUP_ALL ? "all" : "quiet"); return;
    case ROW_KEEP_LOG:
    case ROW_SCENE:    PanelPut(page, attr, v ? "on" : "off"); return;
    default:           return;
    }
}

/* One row the person may choose: its name, and a setting's value between
 * the marks that say ←→ change it — in the warning colour once changed. The
 * chosen row is one bar across the screen. */
static void Choosable(PanelPage *page, const Panel *p, PanelRow row)
{
    int     chosen = p->row == row;
    uint8_t attr   = chosen ? ATTR_CHOSEN : VIDEO_ATTR_DEFAULT;

    PanelPut(page, attr, chosen ? " ▶ " : "   ");
    PanelPut(page, attr, g_row_names[row]);
    if (row < PANEL_SETTINGS) {
        uint32_t pad = 12u;
        for (const char *c = g_row_names[row]; *c && pad; c++) pad--;
        for (; pad; pad--) PanelPut(page, attr, " ");
        PanelPut(page, attr, "‹ ");
        ValueSaid(page, p, row, chosen || p->value[row] == p->came[row] ? attr : VIDEO_ATTR_WARNING);
        PanelPut(page, attr, " ›");
    }
    PanelPut(page, attr, "\n");
}

static void PanelDraw(const Panel *p)
{
    static PanelPage page;

    PanelBegin(&page);
    PanelPut(&page, ATTR_TITLE, " Gangway · the panel\n");
    if (p->why == CROSSING_PANEL_MISSES) {
        PanelPut(&page, VIDEO_ATTR_WARNING, " It opened by itself: the last ");
        PanelNum(&page, VIDEO_ATTR_WARNING, p->misses);
        PanelPut(&page, VIDEO_ATTR_WARNING,
                 " starts did not land. The log the start before kept, startup all or one core may tell why.\n");
    }
    PanelPut(&page, VIDEO_ATTR_DEFAULT, "\n");
    FoundKernel(&page);
    FoundVolume(&page);
    FoundMachine(&page, p);
    FoundCrossings(&page, p);
    PanelPut(&page, ATTR_TITLE, "\n For this start\n");
    for (PanelRow row = ROW_SCREEN; row < PANEL_SETTINGS; row++) Choosable(&page, p, row);
    PanelPut(&page, VIDEO_ATTR_DEFAULT, "\n");
    for (PanelRow row = ROW_START; row <= LastRow(p); row++) Choosable(&page, p, row);
    PanelPut(&page, ATTR_NAME, "\n ↑↓ choose · ←→ change · Enter do\n");
    PanelPut(&page, VIDEO_ATTR_DEFAULT,
             " What is changed here holds for this start only; to keep it, set it in the hub: settings gangway\n");
    if (p->note) {
        PanelPut(&page, VIDEO_ATTR_WARNING, " ");
        PanelPut(&page, VIDEO_ATTR_WARNING, p->note);
        PanelPut(&page, VIDEO_ATTR_WARNING, "\n");
    }
    DrawPage(page.pieces, page.count, 0, 0, DrawRows());
}

/* ------------------------------------------------------------------------- */
/* What the keys do                                                          */
/* ------------------------------------------------------------------------- */

/* A setting's value one step on (`up`) or back, round from the last to the
 * first. */
static void Step(Panel *p, int up)
{
    uint32_t *v    = &p->value[p->row];
    uint32_t  last = 1u;
    if (p->row == ROW_SCREEN) last = p->nsizes;
    if (p->row == ROW_CORES) last = p->machine_cores ? p->machine_cores : PANEL_CORES_MOST;
    if (*v > last) last = *v;   /* asked past what the machine has: kept, and stepped from */
    if (up) *v = *v >= last ? 0u : *v + 1u;
    else *v = *v ? *v - 1u : last;
}

/* Said aloud, then the machine reset as `type` asks; back here only when the
 * firmware would not. */
static void Reset(EFI_RESET_TYPE type, const char *said)
{
    PrintAloud(1);
    Print("Gangway: the panel ");
    Print(said);
    Print("\n");
    PrintAloud(0);
    g_st->runtime_services->reset_system(type, EFI_SUCCESS, 0, NULL);
}

/* OsIndications, with the bit that asks the firmware to start its own
 * settings next (UEFI §8.5.4), and the restart: what could not be done when
 * the firmware would not. */
static const char *FirmwareUi(void)
{
    EFI_GUID              guid = EFI_GLOBAL_VARIABLE_GUID;
    EFI_RUNTIME_SERVICES *rt   = g_st->runtime_services;
    uint64_t              value = 0;
    UINTN                 bytes = sizeof(value);
    uint32_t              attributes = 0;

    if (EFI_ERROR(rt->get_variable(g_os_indications, &guid, &attributes, &bytes, &value)) || bytes != sizeof(value))
        value = 0;
    value |= EFI_OS_INDICATIONS_BOOT_TO_FW_UI;
    if (EFI_ERROR(rt->set_variable(g_os_indications, &guid,
                                   EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
                                       EFI_VARIABLE_RUNTIME_ACCESS,
                                   sizeof(value), &value)))
        return "the firmware would not take the request to start its settings";
    Reset(EfiResetCold, "restarts the machine into the firmware's settings");
    return "the firmware would not restart the machine";
}

/* Enter on the chosen row: 1 when the person goes on with the start. */
static int Do(Panel *p)
{
    switch (p->row) {
    case ROW_START:    return 1;
    case ROW_JOURNAL:  PanelJournalShow(); return 0;
    case ROW_PREVIOUS: PanelPreviousShow(); return 0;
    case ROW_CROSSINGS: PanelCrossingsShow(); return 0;
    case ROW_RESTART:
        Reset(EfiResetCold, "restarts the machine");
        p->note = "the firmware would not restart the machine";
        return 0;
    case ROW_OFF:
        Reset(EfiResetShutdown, "turns the machine off");
        p->note = "the firmware would not turn the machine off";
        return 0;
    case ROW_FIRMWARE: p->note = FirmwareUi(); return 0;
    default:           Step(p, 1); return 0;
    }
}

/* A key on the page: 1 when the person goes on with the start. A note lasts
 * until the next key. */
static int Key(Panel *p, GangwayKey key)
{
    p->note = NULL;
    switch (key) {
    case KEY_UP:    if (p->row > ROW_SCREEN) p->row--; return 0;
    case KEY_DOWN:  if (p->row < LastRow(p)) p->row++; return 0;
    case KEY_HOME:  p->row = ROW_SCREEN; return 0;
    case KEY_END:   p->row = LastRow(p); return 0;
    case KEY_LEFT:
    case KEY_RIGHT: if (p->row < PANEL_SETTINGS) Step(p, key == KEY_RIGHT); return 0;
    case KEY_ENTER: return Do(p);
    default:        return 0;
    }
}

/* ------------------------------------------------------------------------- */
/* Leaving                                                                   */
/* ------------------------------------------------------------------------- */

/* What the person changed, into g_settings for this start, said; the panel
 * off the screen; and the screen chosen again when its size was changed. */
static void Leave(const Panel *p, FbInfo *fb)
{
    uint32_t changed = 0;

    if (p->value[ROW_SCREEN] != p->came[ROW_SCREEN]) {
        uint32_t v               = p->value[ROW_SCREEN];
        g_settings.screen_width  = v ? p->sizes[v - 1u].width : 0;
        g_settings.screen_height = v ? p->sizes[v - 1u].height : 0;
        changed |= GANGWAY_SET_SCREEN;
    }
    if (p->value[ROW_CORES] != p->came[ROW_CORES]) {
        g_settings.cores = p->value[ROW_CORES];
        changed |= GANGWAY_SET_CORES;
    }
    if (p->value[ROW_STARTUP] != p->came[ROW_STARTUP]) {
        g_settings.startup = (uint8_t)p->value[ROW_STARTUP];
        changed |= GANGWAY_SET_STARTUP;
    }
    if (p->value[ROW_KEEP_LOG] != p->came[ROW_KEEP_LOG]) {
        g_settings.keep_log = (uint8_t)p->value[ROW_KEEP_LOG];
        changed |= GANGWAY_SET_KEEP_LOG;
    }
    if (p->value[ROW_SCENE] != p->came[ROW_SCENE]) {
        g_settings.scene = (uint8_t)p->value[ROW_SCENE];
        changed |= GANGWAY_SET_SCENE;
    }
    g_settings.set |= changed;

    PrintAloud(1);
    Print("Gangway: the panel ");
    if (changed) {
        Print("sets for this start: ");
        SettingsList(changed);
        Print("\n");
    } else {
        Print("changes nothing — the start goes on as it was\n");
    }
    PrintAloud(0);

    DrawPage(NULL, 0, 0, 0, DrawRows());
    if (changed & GANGWAY_SET_SCREEN) {
        *fb = QueryGopFramebuffer();
        DrawUse(fb);
    }
}

uint8_t PanelIfAsked(FbInfo *fb, uint32_t misses)
{
    static Panel p;
    int          key = KeyPanelAsked();
    uint8_t      why = misses >= CROSSINGS_MISSES_PANEL ? CROSSING_PANEL_MISSES
                     : key                            ? CROSSING_PANEL_KEY
                                                      : CROSSING_PANEL_NONE;

    if (why == CROSSING_PANEL_NONE) return why;
    PrintAloud(1);
    if (!DrawRows()) {
        Print(why == CROSSING_PANEL_KEY ? "Gangway: the panel is asked for (Space held)"
                                        : "Gangway: the panel would open by itself");
        Print(", but there is no screen to draw it on — the start goes on\n");
        PrintAloud(0);
        return CROSSING_PANEL_NONE;
    }
    if (why == CROSSING_PANEL_KEY) {
        Print("Gangway: the panel is open (Space held)\n");
    } else {
        Print("Gangway: the panel is open by itself — the last ");
        PrintDec(misses);
        Print(" starts did not land\n");
    }
    PrintAloud(0);

    /* The firmware's five minutes are for a loader that hangs, not for a
     * person reading. */
    (void)g_bs->set_watchdog_timer(0, 0, 0, NULL);

    PanelTake(&p, fb, why, misses);
    for (;;) {
        PanelDraw(&p);
        GangwayKey key = KeyNext();
        if (key == KEY_GONE) {
            PrintAloud(1);
            Print("Gangway: the keyboard gives no more keys — the panel goes on with the start\n");
            PrintAloud(0);
            break;
        }
        if (Key(&p, key)) break;
    }
    Leave(&p, fb);
    Moment("panel");
    return why;
}
