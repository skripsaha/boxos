/*
 * Gangway — the panel. Asked for by holding Space while the machine starts
 * (KeyPanelAsked, once, after the kernel and the screen are chosen), or open
 * by itself when the last starts did not land (the crossings,
 * gangway_crossings.c). A page of rows: Start, and the pages they open — the
 * start's options, the same seven settings gangway.settings sets, changed for
 * this one start and taken as the file's are; the journals: this start's, the
 * log the kernel kept of the start before and the crossings kept
 * (gangway_panel_log.c); and what the loader found and chose — then Restart,
 * Turn off, and a restart into the firmware's own settings. How it looks is
 * gangway_panel_look.c's: this file says what each page holds and what each
 * key does.
 *
 * Nothing in it waits on a clock: the firmware's watchdog is stopped when it
 * opens, keys are waited for on the console's event, and it stays until the
 * person chooses. It writes nothing to the volume — a choice kept is set in
 * the hub (`settings gangway`), and the options page says so. What it says goes
 * into the journal and down COM1 both (PrintAloud): a machine standing at the
 * panel says why on the wire.
 */

#include "gangway.h"
#include "boarding_pass.h"

/* The settings the options page changes, in the order of the declaration
 * (boxos_gangway_settings.h). */
typedef enum {
    SET_SCREEN,
    SET_CORES,
    SET_STARTUP,
    SET_KEEP_LOG,
    SET_SCENE,
    SET_TEXT,
    SET_THEME,
    PANEL_SETTINGS,
} PanelSetting;

/* Every row a page may hold: a setting's row is its setting's place on from
 * ROW_SCREEN; an info row only says. */
typedef enum {
    ROW_START,
    ROW_OPTIONS,
    ROW_JOURNALS,
    ROW_ABOUT,
    ROW_RESTART,
    ROW_OFF,
    ROW_FIRMWARE,
    ROW_SCREEN,
    ROW_CORES,
    ROW_STARTUP,
    ROW_KEEP_LOG,
    ROW_SCENE,
    ROW_TEXT,
    ROW_THEME,
    ROW_JOURNAL,
    ROW_PREVIOUS,
    ROW_CROSSINGS,
    ROW_KERNEL,
    ROW_VOLUME,
    ROW_FOUND_SCREEN,
    ROW_PROCESSORS,
    ROW_STARTS,
    ROW_BACK,
    ROW_RULE,
} PanelRow;

static const char *const g_names[] = {
    "Start", "Start options", "Journals", "About this machine", "Restart", "Turn off", "Firmware settings",
    "Screen", "Cores", "Startup", "Keep-log", "Scene", "Text size", "Theme",
    "This start's journal", "The previous start's log", "The crossings",
    "Kernel", "Volume", "Screen", "Processors", "Starts", "Back", NULL,
};

/* The pages, and the rows each holds in its order. */
typedef enum {
    SHEET_MAIN,
    SHEET_OPTIONS,
    SHEET_JOURNALS,
    SHEET_ABOUT,
} PanelSheet;

static const PanelRow g_main[]     = { ROW_START, ROW_OPTIONS, ROW_JOURNALS, ROW_ABOUT, ROW_RULE,
                                       ROW_RESTART, ROW_OFF, ROW_FIRMWARE };
static const PanelRow g_options[]  = { ROW_SCREEN, ROW_CORES, ROW_STARTUP, ROW_KEEP_LOG, ROW_SCENE,
                                       ROW_TEXT, ROW_THEME, ROW_RULE, ROW_BACK };
static const PanelRow g_journals[] = { ROW_JOURNAL, ROW_PREVIOUS, ROW_CROSSINGS, ROW_RULE, ROW_BACK };
static const PanelRow g_about[]    = { ROW_KERNEL, ROW_VOLUME, ROW_FOUND_SCREEN, ROW_PROCESSORS, ROW_STARTS,
                                       ROW_RULE, ROW_BACK };

#define PANEL_SIZES_MOST  48u
/* The most cores the declaration takes (cores number from 1 to 256). */
#define PANEL_CORES_MOST  256u
/* The text sizes text-size takes, auto first. */
static const uint32_t g_text_sizes[] = { 0u, 16u, 24u, 32u };

typedef struct {
    uint8_t     why;                      /* CROSSING_PANEL_KEY or _MISSES */
    uint32_t    misses;                   /* starts in a row that did not land */
    FbInfo      fb;                       /* the screen as the journey chose it */
    GopSize     sizes[PANEL_SIZES_MOST];
    uint32_t    nsizes;
    uint32_t    machine_cores;            /* as the firmware counts them, 0 when it does not say */
    int         firmware_ui;              /* the firmware can be asked to start its settings */
    /* Each setting's value — screen: 0 auto, else sizes[v - 1]; cores: 0 all,
     * else so many; startup: BOARDING_STARTUP_*; keep-log, scene: 1 on;
     * text-size: px, 0 auto; theme: GANGWAY_THEME_* — and as the panel found
     * it. */
    uint32_t    value[PANEL_SETTINGS];
    uint32_t    came[PANEL_SETTINGS];
    PanelSheet  sheet;
    uint32_t    chosen;                   /* the row chosen, its place on the page */
    uint32_t    opened;                   /* the main page's row that opened the one shown */
    const char *note;                     /* what the last thing done could not do */
} Panel;

static CHAR16 g_os_indications[]           = u"OsIndications";
static CHAR16 g_os_indications_supported[] = u"OsIndicationsSupported";

/* ------------------------------------------------------------------------- */
/* A page of text (gangway_panel_log.c's)                                    */
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

/* ------------------------------------------------------------------------- */
/* Words of a row                                                            */
/* ------------------------------------------------------------------------- */

typedef struct {
    char     text[64];
    uint32_t used;
} PanelWords;

static void WordsPut(PanelWords *w, const char *text)
{
    while (*text && w->used < sizeof(w->text) - 1u) w->text[w->used++] = *text++;
    w->text[w->used] = '\0';
}

static void WordsNum(PanelWords *w, uint64_t n)
{
    char digits[21];
    int  at = 20;
    digits[at] = '\0';
    do {
        digits[--at] = (char)('0' + n % 10u);
        n /= 10u;
    } while (n);
    WordsPut(w, digits + at);
}

/* "2.0 GiB", "155 MiB", "1.5 MiB". */
static void WordsBytes(PanelWords *w, uint64_t bytes)
{
    const uint64_t mib = 1024u * 1024u, gib = 1024u * mib;
    if (bytes < gib) {
        WordsNum(w, bytes / mib);
        if (bytes < 10u * mib) {
            WordsPut(w, ".");
            WordsNum(w, bytes % mib * 10u / mib);
        }
        WordsPut(w, " MiB");
        return;
    }
    WordsNum(w, bytes / gib);
    WordsPut(w, ".");
    WordsNum(w, bytes % gib * 10u / gib);
    WordsPut(w, " GiB");
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
                p->came[SET_SCREEN] = i + 1u;
    }
    p->came[SET_CORES]    = g_settings.cores;
    p->came[SET_STARTUP]  = g_settings.startup;
    p->came[SET_KEEP_LOG] = g_settings.keep_log;
    p->came[SET_SCENE]    = g_settings.scene;
    p->came[SET_TEXT]     = g_settings.text_size;
    p->came[SET_THEME]    = g_settings.theme;
    MemCopy(p->value, p->came, sizeof(p->value));
    p->sheet  = SHEET_MAIN;
    p->chosen = 0;
}

/* ------------------------------------------------------------------------- */
/* The pages                                                                 */
/* ------------------------------------------------------------------------- */

static const PanelRow *SheetRows(const Panel *p, uint32_t *count)
{
    switch (p->sheet) {
    case SHEET_OPTIONS:  *count = sizeof(g_options) / sizeof(g_options[0]); return g_options;
    case SHEET_JOURNALS: *count = sizeof(g_journals) / sizeof(g_journals[0]); return g_journals;
    case SHEET_ABOUT:    *count = sizeof(g_about) / sizeof(g_about[0]); return g_about;
    default:
        *count = sizeof(g_main) / sizeof(g_main[0]) - (p->firmware_ui ? 0u : 1u);
        return g_main;
    }
}

static int IsInfo(PanelRow row)
{
    return row >= ROW_KERNEL && row <= ROW_STARTS;
}

static int IsSetting(PanelRow row)
{
    return row >= ROW_SCREEN && row <= ROW_THEME;
}

static int Choosable(PanelRow row)
{
    return row != ROW_RULE && !IsInfo(row);
}

/* A setting's value in words. */
static void ValueSaid(PanelWords *w, const Panel *p, PanelSetting s)
{
    uint32_t v = p->value[s];
    switch (s) {
    case SET_SCREEN:
        if (!v) {
            WordsPut(w, "auto");
            return;
        }
        WordsNum(w, p->sizes[v - 1u].width);
        WordsPut(w, "x");
        WordsNum(w, p->sizes[v - 1u].height);
        return;
    case SET_CORES:
        if (v) WordsNum(w, v);
        else WordsPut(w, "all");
        return;
    case SET_STARTUP:  WordsPut(w, v == BOARDING_STARTUP_ALL ? "all" : "quiet"); return;
    case SET_KEEP_LOG:
    case SET_SCENE:    WordsPut(w, v ? "on" : "off"); return;
    case SET_TEXT:
        if (v) {
            WordsNum(w, v);
            WordsPut(w, " px");
        } else {
            WordsPut(w, "auto");
        }
        return;
    case SET_THEME:    WordsPut(w, v == GANGWAY_THEME_LIGHT ? "light" : "dark"); return;
    default:           return;
    }
}

/* What an info row says of what the loader found and chose. */
static void FoundSaid(PanelWords *w, const Panel *p, PanelRow row)
{
    static const char hex[] = "0123456789abcdef";
    const LecternKernel *k = KernelChosen();
    switch (row) {
    case ROW_KERNEL:
        WordsPut(w, k->says == ATTEST_HELD ? "attested by the book, " : "not attested, ");
        WordsBytes(w, k->bytes);
        return;
    case ROW_VOLUME:
        for (uint32_t i = 0; i < 4u; i++) {
            char pair[3] = { hex[g_volume_uuid[i] >> 4], hex[g_volume_uuid[i] & 15u], '\0' };
            WordsPut(w, pair);
        }
        WordsPut(w, "…, ");
        WordsBytes(w, g_volume_end * VOLUME_BLOCK_BYTES);
        return;
    case ROW_FOUND_SCREEN:
        WordsNum(w, p->fb.width);
        WordsPut(w, "x");
        WordsNum(w, p->fb.height);
        return;
    case ROW_PROCESSORS:
        if (p->machine_cores) WordsNum(w, p->machine_cores);
        else WordsPut(w, "the firmware does not say");
        return;
    case ROW_STARTS:
        if (CrossingsNone()) {
            WordsPut(w, "none kept on this medium");
            return;
        }
        WordsPut(w, "this is start ");
        WordsNum(w, CrossingsNext());
        if (p->misses) {
            WordsPut(w, "; ");
            WordsNum(w, p->misses);
            WordsPut(w, " before did not land");
        }
        return;
    default: return;
    }
}

/* The page as it stands, handed to how the panel looks. */
static void PanelDraw(const Panel *p)
{
    static PanelWords words[16];
    PanelLookRow      rows[16];
    uint32_t          count;
    const PanelRow   *list = SheetRows(p, &count);
    static const char *const titles[] = { NULL, "Start options", "Journals", "About this machine" };
    static const char *const hints[]  = {
        "↑↓ choose · Enter open",
        "↑↓ choose · ←→ change · Esc back\nfor this start only — to keep it: settings gangway",
        "↑↓ choose · Enter open · Esc back",
        "Esc back",
    };

    for (uint32_t i = 0; i < count; i++) {
        PanelRow row = list[i];
        MemZero(&rows[i], sizeof(rows[i]));
        words[i].used    = 0;
        words[i].text[0] = '\0';
        rows[i].label    = g_names[row];
        rows[i].more     = row == ROW_OPTIONS || row == ROW_JOURNALS || row == ROW_ABOUT;
        rows[i].info     = (uint8_t)IsInfo(row);
        if (IsSetting(row)) {
            PanelSetting s = (PanelSetting)(row - ROW_SCREEN);
            ValueSaid(&words[i], p, s);
            rows[i].value   = words[i].text;
            rows[i].changed = p->value[s] != p->came[s];
        } else if (IsInfo(row)) {
            FoundSaid(&words[i], p, row);
            rows[i].value = words[i].text;
        }
    }
    PanelLookPage(titles[p->sheet], rows, count, p->chosen, p->note, hints[p->sheet]);
}

/* The logo drawn again, and what is said under it: when the panel opened by
 * itself, why; when the kernel is not attested, that. */
static int PanelAgain(const Panel *p)
{
    static PanelWords said;
    const char       *line = NULL, *under = NULL;
    if (p->why == CROSSING_PANEL_MISSES) {
        said.used = 0;
        WordsPut(&said, "The last ");
        WordsNum(&said, p->misses);
        WordsPut(&said, " starts did not finish.");
        line  = said.text;
        under = "Start again, or open Journals to see why.";
    } else if (KernelChosen()->says != ATTEST_HELD) {
        line  = "The kernel is not attested by the book.";
        under = "Nothing shows it broken; it is loaded all the same.";
    }
    return PanelLookBegin(&p->fb, line, under);
}

/* ------------------------------------------------------------------------- */
/* What the keys do                                                          */
/* ------------------------------------------------------------------------- */

/* The next row that may be chosen, down or up, staying where it is at the
 * page's end. */
static void Move(Panel *p, int down)
{
    uint32_t        count;
    const PanelRow *list = SheetRows(p, &count);
    for (uint32_t at = p->chosen;;) {
        if (down ? at + 1u >= count : at == 0) return;
        at = down ? at + 1u : at - 1u;
        if (Choosable(list[at])) {
            p->chosen = at;
            return;
        }
    }
}

static void Ends(Panel *p, int last)
{
    uint32_t        count;
    const PanelRow *list = SheetRows(p, &count);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t at = last ? count - 1u - i : i;
        if (Choosable(list[at])) {
            p->chosen = at;
            return;
        }
    }
}

static void Open(Panel *p, PanelSheet sheet)
{
    p->opened = p->chosen;
    p->sheet  = sheet;
    Ends(p, 0);
}

static void Back(Panel *p)
{
    p->sheet  = SHEET_MAIN;
    p->chosen = p->opened;
}

/* A setting's value one step on (`up`) or back, round from the last to the
 * first; the panel's own letters and colours taken at once. */
static void Step(Panel *p, PanelSetting s, int up)
{
    uint32_t *v = &p->value[s];
    if (s == SET_TEXT) {
        const uint32_t n = sizeof(g_text_sizes) / sizeof(g_text_sizes[0]);
        uint32_t       at = 0;
        while (at < n && g_text_sizes[at] != *v) at++;
        at = at == n ? 0 : up ? (at + 1u) % n : (at + n - 1u) % n;
        *v = g_text_sizes[at];
    } else {
        uint32_t last = 1u;
        if (s == SET_SCREEN) last = p->nsizes;
        if (s == SET_CORES) last = p->machine_cores ? p->machine_cores : PANEL_CORES_MOST;
        if (*v > last) last = *v;   /* asked past what the machine has: kept, and stepped from */
        if (up) *v = *v >= last ? 0u : *v + 1u;
        else *v = *v ? *v - 1u : last;
    }
    if (s == SET_TEXT || s == SET_THEME) {
        g_settings.text_size = p->value[SET_TEXT];
        g_settings.theme     = (uint8_t)p->value[SET_THEME];
        DrawUse(&p->fb);
        (void)PanelAgain(p);
    }
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
    uint32_t        count;
    const PanelRow *list = SheetRows(p, &count);
    PanelRow        row  = list[p->chosen];
    if (IsSetting(row)) {
        Step(p, (PanelSetting)(row - ROW_SCREEN), 1);
        return 0;
    }
    switch (row) {
    case ROW_START:     return 1;
    case ROW_OPTIONS:   Open(p, SHEET_OPTIONS); return 0;
    case ROW_JOURNALS:  Open(p, SHEET_JOURNALS); return 0;
    case ROW_ABOUT:     Open(p, SHEET_ABOUT); return 0;
    case ROW_BACK:      Back(p); return 0;
    case ROW_JOURNAL:   PanelJournalShow(); (void)PanelAgain(p); return 0;
    case ROW_PREVIOUS:  PanelPreviousShow(); (void)PanelAgain(p); return 0;
    case ROW_CROSSINGS: PanelCrossingsShow(); (void)PanelAgain(p); return 0;
    case ROW_RESTART:
        Reset(EfiResetCold, "restarts the machine");
        p->note = "the firmware would not restart the machine";
        return 0;
    case ROW_OFF:
        Reset(EfiResetShutdown, "turns the machine off");
        p->note = "the firmware would not turn the machine off";
        return 0;
    case ROW_FIRMWARE:  p->note = FirmwareUi(); return 0;
    default:            return 0;
    }
}

/* A key on the page: 1 when the person goes on with the start. A note lasts
 * until the next key. */
static int Key(Panel *p, GangwayKey key)
{
    uint32_t        count;
    const PanelRow *list = SheetRows(p, &count);
    PanelRow        row  = list[p->chosen];
    p->note = NULL;
    switch (key) {
    case KEY_UP:    Move(p, 0); return 0;
    case KEY_DOWN:  Move(p, 1); return 0;
    case KEY_HOME:  Ends(p, 0); return 0;
    case KEY_END:   Ends(p, 1); return 0;
    case KEY_LEFT:
    case KEY_RIGHT: if (IsSetting(row)) Step(p, (PanelSetting)(row - ROW_SCREEN), key == KEY_RIGHT); return 0;
    case KEY_BACK:  if (p->sheet != SHEET_MAIN) Back(p); return 0;
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

    if (p->value[SET_SCREEN] != p->came[SET_SCREEN]) {
        uint32_t v               = p->value[SET_SCREEN];
        g_settings.screen_width  = v ? p->sizes[v - 1u].width : 0;
        g_settings.screen_height = v ? p->sizes[v - 1u].height : 0;
        changed |= GANGWAY_SET_SCREEN;
    }
    if (p->value[SET_CORES] != p->came[SET_CORES]) {
        g_settings.cores = p->value[SET_CORES];
        changed |= GANGWAY_SET_CORES;
    }
    if (p->value[SET_STARTUP] != p->came[SET_STARTUP]) {
        g_settings.startup = (uint8_t)p->value[SET_STARTUP];
        changed |= GANGWAY_SET_STARTUP;
    }
    if (p->value[SET_KEEP_LOG] != p->came[SET_KEEP_LOG]) {
        g_settings.keep_log = (uint8_t)p->value[SET_KEEP_LOG];
        changed |= GANGWAY_SET_KEEP_LOG;
    }
    if (p->value[SET_SCENE] != p->came[SET_SCENE]) {
        g_settings.scene = (uint8_t)p->value[SET_SCENE];
        changed |= GANGWAY_SET_SCENE;
    }
    g_settings.text_size = p->value[SET_TEXT];
    g_settings.theme     = (uint8_t)p->value[SET_THEME];
    if (p->value[SET_TEXT] != p->came[SET_TEXT]) changed |= GANGWAY_SET_TEXT;
    if (p->value[SET_THEME] != p->came[SET_THEME]) changed |= GANGWAY_SET_THEME;
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

    PanelLookEnd();
    DrawPage(NULL, 0, 0, 0, DrawRows());
    if (changed & GANGWAY_SET_SCREEN) {
        *fb = QueryGopFramebuffer();
        DrawUse(fb);
    }
}

uint8_t PanelIfAsked(FbInfo *fb, uint32_t misses)
{
    static Panel p;
    int          key = KeyPanelAsked(misses < CROSSINGS_MISSES_PANEL && DrawRows());
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
    if (!PanelAgain(&p)) {
        PrintAloud(1);
        Print("Gangway: the firmware gives no room to draw the panel — the start goes on\n");
        PrintAloud(0);
        return CROSSING_PANEL_NONE;
    }
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
