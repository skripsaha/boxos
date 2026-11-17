/*
 * Gangway — how the panel looks (gangway_panel.c says what it holds and does).
 *
 * The screen in two: on the left Duppo's logo, duppo-word still, kept in the
 * loader itself (gangway_logo.asm) and drawn once by the scene's own player;
 * on the right a page of rows, the block of them in the middle of the screen's
 * height. The row chosen stands on a plashka of Duppo's own sphere — the
 * light falling down it as it falls down him, a step darker — with his
 * symbol from the system's font before its name, both in white; the others
 * are quiet, a rule between groups. Under the logo nothing, unless there is
 * something to say: then a line, and a quieter one under it.
 *
 * Everything is drawn into a canvas as large as the screen, in memory the
 * firmware gives while the panel stands, and shown from it: the whole once,
 * then the page's column alone on every key. The letters are the person's
 * text size and font, or a size the screen holds comfortably; the colours
 * their theme's, each colour they set standing over it — the plashka's light
 * falls the same way down whatever colour they choose. With the logo off the
 * page stands in the middle of the screen, and what would be said under the
 * logo above it.
 */

#include "gangway.h"
#include "boxos_imprint.h"
#include "boxos_scene_play.h"
#include "boxos_symbols.h"
#include "boxos_utf8.h"

extern const uint8_t GangwayLogoDark[], GangwayLogoDarkEnd[];
extern const uint8_t GangwayLogoLight[], GangwayLogoLightEnd[];

#define ONE  BOX_BRUSH_ONE
#define UNIT BOX_BRUSH_UNIT

/* The theme's colours, 0x00RRGGBB. */
typedef struct {
    uint32_t paper;
    uint32_t ink;       /* a page's title */
    uint32_t quiet;     /* a row not chosen */
    uint32_t faint;     /* the marks, the hints, an info row's name */
    uint32_t rule;
    uint32_t told;      /* a value changed, the line under the logo, a note */
} LookInks;

static const LookInks g_dark  = { 0x000000u, 0xFFFFFFu, 0xA49CA6u, 0x6A636Cu, 0x2A2430u, 0xF2A0BFu };
static const LookInks g_light = { 0xFFFFFFu, 0x2E1A26u, 0x6F6670u, 0x9A929Cu, 0xE2DADFu, 0xB0476Fu };

/* The plashka: Duppo's sphere top to bottom, a step darker than the logo. */
static const BoxBrushStop g_plashka[] = {
    { 0u, 0xFFCF6089u }, { UNIT / 5u, 0xFFD8678Fu }, { UNIT / 2u, 0xFFCB5D87u },
    { UNIT * 4u / 5u, 0xFFA0406Fu }, { UNIT, 0xFF7E2457u },
};

typedef struct {
    FbInfo               fb;
    LookInks             own;             /* the theme's, the person's colours over it */
    BoxBrushStop         plashka[5];
    int                  logo;            /* Duppo's logo beside the page */
    const char          *said, *under;    /* what is said under it — over the page with no logo */
    EFI_PHYSICAL_ADDRESS room;
    UINTN                pages;
    BoxBrushCanvas       canvas;
    BoxBrushEdge        *edges;
    const LookInks      *inks;
    uint32_t             size, small;   /* the imprint's sizes: the rows', the hints' */
    int32_t              ch, cw, sh;    /* a row's cell tall and wide, a hint's tall */
    int32_t              lx, ly, lw, lh;   /* the logo's room */
    int32_t              mx, mw;           /* the column: where its names begin, how wide */
    int32_t              left;             /* the column's room begins here, to the screen's right */
    int                  shown;            /* the whole canvas is on the screen */
} Look;

static Look g_look;

static UINTN LookPages(uint64_t bytes)
{
    return (UINTN)((bytes + 4095u) / 4096u);
}

static uint8_t *Carve(uint8_t **at, uint64_t bytes)
{
    uint8_t *got = *at;
    *at += (bytes + 15u) & ~15ull;
    return got;
}

static uint64_t Length(const char *text)
{
    uint64_t n = 0;
    while (text[n]) n++;
    return n;
}

/* Pixels across a text in the imprint's cells, a character a cell. */
static int32_t Across(const char *text, uint32_t size)
{
    int32_t n = 0;
    for (uint64_t at = 0, bytes = Length(text); at < bytes; n++) {
        uint32_t c;
        at += BoxUtf8Next(text + at, bytes - at, &c);
    }
    return n * (int32_t)DrawImprint()->Sizes[size].Width;
}

static void Say(uint32_t size, int32_t x, int32_t y, const char *text, uint32_t rgb)
{
    BoxBrushText(&g_look.canvas, DrawImprint(), size, x, y, text, Length(text), 0xFF000000u | rgb, 255);
}

static void Rounded(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, const BoxBrushPaint *paint)
{
    BoxBrushOutline O;
    static const BoxBrushMatrix plain = { BOX_BRUSH_UNIT, 0, 0, BOX_BRUSH_UNIT, 0, 0 };
    BoxBrushBegin(&O, g_look.edges, BOX_SCENE_EDGES, &plain);
    BoxBrushAddRounded(&O, x * ONE, y * ONE, w * ONE, h * ONE, r * ONE, 0);
    BoxBrushFill(&g_look.canvas, &O, paint, 255);
}

static void Solid(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint32_t rgb)
{
    const BoxBrushPaint paint = { BOX_BRUSH_PAINT_SOLID, 0xFF000000u | rgb, NULL, 0,
                                  { BOX_BRUSH_UNIT, 0, 0, BOX_BRUSH_UNIT, 0, 0 }, NULL, 0, 0, 0 };
    Rounded(x, y, w, h, r, &paint);
}

/* `rgb` lit `per` percent: past 100 toward white, under it toward black. */
static uint32_t Shade(uint32_t rgb, uint32_t per)
{
    rgb &= 0x00FFFFFFu;
    return per >= 100u ? BoxColorBetween(rgb, 0x00FFFFFFu, (per - 100u) * BOX_COLOR_WAY / 100u)
                       : BoxColorBetween(0, rgb, per * BOX_COLOR_WAY / 100u);
}

/* Whether `rgb` is a light colour, by how bright it looks. */
static int Light(uint32_t rgb)
{
    return ((rgb >> 16 & 255u) * 299u + (rgb >> 8 & 255u) * 587u + (rgb & 255u) * 114u) / 1000u >= 128u;
}

/* The colours as the person has them: the theme's, each they set over it; the
 * plashka Duppo's own, or their choice with his light falling down it. */
static void LookInksTake(void)
{
    const uint32_t set = g_settings.colors_set;
    const uint32_t *c  = g_settings.colors;
    g_look.own = g_settings.theme == GANGWAY_THEME_LIGHT ? g_light : g_dark;
    if (set & 1u << GANGWAY_COLOR_PAGE) g_look.own.paper = c[GANGWAY_COLOR_PAGE];
    if (set & 1u << GANGWAY_COLOR_TEXT) g_look.own.quiet = c[GANGWAY_COLOR_TEXT];
    if (set & 1u << GANGWAY_COLOR_TITLE) g_look.own.ink = c[GANGWAY_COLOR_TITLE];
    if (set & 1u << GANGWAY_COLOR_HINT) {
        g_look.own.faint = c[GANGWAY_COLOR_HINT];
        g_look.own.rule  = c[GANGWAY_COLOR_HINT];
    }
    if (set & 1u << GANGWAY_COLOR_NOTE) g_look.own.told = c[GANGWAY_COLOR_NOTE];
    g_look.inks = &g_look.own;
    MemCopy(g_look.plashka, g_plashka, sizeof(g_plashka));
    if (set & 1u << GANGWAY_COLOR_CHOICE) {
        static const uint32_t per[5] = { 102u, 107u, 100u, 78u, 60u };
        for (uint32_t i = 0; i < 5u; i++) g_look.plashka[i].Color = 0xFF000000u | Shade(c[GANGWAY_COLOR_CHOICE], per[i]);
    }
}

/* The size the rows are drawn in: the person's text size, or by the screen's
 * height — and never one whose longest row would not fit the column. */
static uint32_t LookSize(uint32_t height, int32_t column)
{
    uint32_t size = height < 700u ? 0u : height < 1000u ? 1u : 2u;
    for (uint32_t s = 0; g_settings.text_size && s < DrawImprint()->SizeCount; s++)
        if (DrawImprint()->Sizes[s].Height == g_settings.text_size) size = s;
    if (size >= DrawImprint()->SizeCount) size = DrawImprint()->SizeCount - 1u;
    while (size && 36 * (int32_t)DrawImprint()->Sizes[size].Width > column) size--;
    return size;
}

/* Where everything goes on a screen `w` by `h`: side by side on a wide one,
 * the logo over the page on one standing up. */
static void LookLay(uint32_t w, uint32_t h)
{
    if (!g_look.logo) {
        const int wide = (uint64_t)w * 10u >= (uint64_t)h * 12u;
        g_look.lx = g_look.ly = g_look.lw = g_look.lh = 0;
        g_look.mw   = (int32_t)(w * (wide ? 40u : 76u) / 100u);
        g_look.size = LookSize(h, g_look.mw);
        g_look.ch   = (int32_t)DrawImprint()->Sizes[g_look.size].Height;
        g_look.mx   = ((int32_t)w - g_look.mw - 2 * g_look.ch) / 2 + 2 * g_look.ch;
    } else if ((uint64_t)w * 10u >= (uint64_t)h * 12u) {
        g_look.lx = (int32_t)(w * 6u / 100u);
        g_look.lw = (int32_t)(w * 38u / 100u);
        g_look.ly = 0;
        g_look.lh = (int32_t)h;
        g_look.mx = (int32_t)(w * 54u / 100u);
        g_look.mw = (int32_t)(w * 36u / 100u);
    } else {
        g_look.lx = (int32_t)(w * 10u / 100u);
        g_look.lw = (int32_t)(w * 80u / 100u);
        g_look.ly = (int32_t)(h * 2u / 100u);
        g_look.lh = (int32_t)(h * 36u / 100u);
        g_look.mx = (int32_t)(w * 14u / 100u);
        g_look.mw = (int32_t)(w * 76u / 100u);
    }
    if (g_look.logo) g_look.size = LookSize(h, g_look.mw);
    g_look.small = g_look.size ? g_look.size - 1u : 0u;
    g_look.ch    = (int32_t)DrawImprint()->Sizes[g_look.size].Height;
    g_look.cw    = (int32_t)DrawImprint()->Sizes[g_look.size].Width;
    g_look.sh    = (int32_t)DrawImprint()->Sizes[g_look.small].Height;
    g_look.left  = g_look.logo ? g_look.mx - 3 * g_look.ch : 0;
}

/* The logo in its room: the scene's first frame, its paper the theme's. */
static void LookLogo(const BoxScene *scene, uint8_t **at)
{
    const uint32_t figures = scene->Head->Figures;
    BoxScenePose  *start = (BoxScenePose *)(void *)Carve(at, (uint64_t)figures * sizeof(BoxScenePose));
    BoxScenePose  *now   = (BoxScenePose *)(void *)Carve(at, (uint64_t)figures * sizeof(BoxScenePose));
    BoxSceneLaid  *laid  = (BoxSceneLaid *)(void *)Carve(at, (uint64_t)figures * sizeof(BoxSceneLaid));
    BoxSceneLaid  *shown = (BoxSceneLaid *)(void *)Carve(at, (uint64_t)figures * sizeof(BoxSceneLaid));

    BoxBrushCanvas room = g_look.canvas;
    room.Pixels      = g_look.canvas.Pixels + (uint64_t)g_look.ly * g_look.canvas.Stride + (uint64_t)g_look.lx;
    room.Width       = (uint32_t)g_look.lw;
    room.Height      = (uint32_t)g_look.lh;
    room.Clip.X      = 0;
    room.Clip.Y      = 0;
    room.Clip.Width  = g_look.lw;
    room.Clip.Height = g_look.lh;
    BoxScenePlayer player;
    BoxBrushRect   whole;
    BoxScenePlayBegin(&player, scene, &room, start, now, laid, shown, g_look.edges);
    BoxScenePlayDraw(&player, &whole, BoxScenePlayAt(&player, 0, &whole, 1));
}

/* What is said under the logo: a line, and a quieter one under it. */
static void LookSaid(const char *said, const char *under)
{
    const int32_t middle = g_look.lx + g_look.lw / 2;
    int32_t       y      = g_look.ly + g_look.lh / 2 + g_look.lw * 36 / 100;
    if (y + 3 * g_look.ch > (int32_t)g_look.fb.height) y = (int32_t)g_look.fb.height - 3 * g_look.ch;
    if (said) Say(g_look.size, middle - Across(said, g_look.size) / 2, y, said, g_look.inks->told);
    if (under) Say(g_look.small, middle - Across(under, g_look.small) / 2, y + g_look.ch * 3 / 2, under, g_look.inks->faint);
}

int PanelLookBegin(const FbInfo *fb, const char *said, const char *under)
{
    PanelLookEnd();
    if (!fb->addr || !fb->width || !fb->height) return 0;
    MemCopy(&g_look.fb, fb, sizeof(*fb));
    g_look.logo  = g_settings.logo;
    g_look.said  = said;
    g_look.under = under;
    LookInksTake();
    LookLay(fb->width, fb->height);

    /* The logo drawn for the page it stands on: its light variant on a light
     * page, and the page's own colour as its paper. */
    const int      light = Light(g_look.own.paper);
    const uint8_t *logo  = light ? GangwayLogoLight : GangwayLogoDark;
    const uint64_t size  = (uint64_t)((light ? GangwayLogoLightEnd : GangwayLogoDarkEnd) - logo);
    BoxScene       scene;
    const char    *bad   = g_look.logo ? BoxSceneRead(logo, size, &scene) : NULL;
    const uint64_t figures = g_look.logo && !bad ? scene.Head->Figures : 0u;
    const uint64_t pixels = (uint64_t)fb->width * fb->height * 4u;
    const uint64_t row    = ((uint64_t)fb->width + 2u) * 4u;
    const uint64_t edges  = (uint64_t)BOX_SCENE_EDGES * sizeof(BoxBrushEdge);
    const uint64_t poses  = figures * 2u * (sizeof(BoxScenePose) + sizeof(BoxSceneLaid));
    const uint64_t copy   = g_look.logo ? size : 0u;
    const uint64_t bytes  = pixels + row + edges + poses + copy + 8u * 16u;
    g_look.pages = LookPages(bytes);
    if (EFI_ERROR(g_bs->allocate_pages(AllocateAnyPages, EfiLoaderData, g_look.pages, &g_look.room))) {
        g_look.room = 0;
        return 0;
    }
    uint8_t *at = (uint8_t *)(uintptr_t)g_look.room;
    g_look.canvas.Pixels      = (uint32_t *)(void *)Carve(&at, pixels);
    g_look.canvas.Row         = (int32_t *)(void *)Carve(&at, row);
    g_look.canvas.Width       = fb->width;
    g_look.canvas.Height      = fb->height;
    g_look.canvas.Stride      = fb->width;
    g_look.canvas.Clip.X      = 0;
    g_look.canvas.Clip.Y      = 0;
    g_look.canvas.Clip.Width  = (int32_t)fb->width;
    g_look.canvas.Clip.Height = (int32_t)fb->height;
    g_look.edges              = (BoxBrushEdge *)(void *)Carve(&at, edges);
    BoxBrushClear(&g_look.canvas, g_look.canvas.Clip, g_look.inks->paper);
    if (bad) {
        Print("Gangway: the panel's logo will not read — ");
        Print(bad);
        Print("; the panel is drawn without it\n");
    } else if (g_look.logo) {
        uint8_t *own = Carve(&at, copy);
        MemCopy(own, logo, copy);
        if (!BoxSceneRead(own, copy, &scene)) {
            ((BoxSceneHead *)(uintptr_t)scene.Head)->Paper = g_look.own.paper;
            LookLogo(&scene, &at);
        }
        LookSaid(said, under);
    }
    g_look.shown = 0;
    return 1;
}

/* A row: its name, a setting's value at the column's right — between the
 * marks that say ←→ change it when chosen — or the mark that it opens a page;
 * an info row its name quiet in a column of its own and what it says after. */
static void LookRow(const PanelLookRow *r, int chosen, int32_t y, int32_t rh, int32_t names)
{
    const LookInks *k  = g_look.inks;
    const int32_t   ty = y + (rh - g_look.ch) / 2;
    if (chosen) {
        const int32_t px = g_look.mx - 2 * g_look.ch - g_look.ch / 3, pw = g_look.mw + 2 * g_look.ch + g_look.ch / 3;
        const int32_t py = y + rh / 10, ph = rh * 8 / 10;
        const BoxBrushPaint plashka = { BOX_BRUSH_PAINT_LINEAR, 0, g_look.plashka, 5,
                                        { 0, ph * ONE, pw * ONE, 0, px * ONE, py * ONE }, NULL, 0, 0, 0 };
        Rounded(px, py, pw, ph, ph / 2, &plashka);
        Say(g_look.size, g_look.mx - g_look.ch - g_look.cw, ty, BOXOS_SYMBOL_DUPPO_TEXT, 0xFFFFFFu);
    }
    const uint32_t name = chosen ? 0xFFFFFFu : r->info ? k->faint : k->quiet;
    Say(g_look.size, g_look.mx, ty, r->label, name);
    if (r->info && r->value) {
        Say(g_look.size, g_look.mx + names, ty, r->value, k->quiet);
    } else if (r->value) {
        char        between[80];
        const char *shown = r->value;
        if (chosen) {
            uint32_t n = 0;
            for (const char *c = "‹ "; *c; c++) between[n++] = *c;
            for (const char *c = r->value; *c && n < sizeof(between) - 6u; c++) between[n++] = *c;
            for (const char *c = " ›"; *c; c++) between[n++] = *c;
            between[n] = '\0';
            shown = between;
        }
        const uint32_t ink = chosen ? 0xFFFFFFu : r->changed ? k->told : k->quiet;
        Say(g_look.size, g_look.mx + g_look.mw - g_look.ch - Across(shown, g_look.size), ty, shown, ink);
    }
    if (r->more) Say(g_look.size, g_look.mx + g_look.mw - 2 * g_look.ch, ty, "›", chosen ? 0xFFFFFFu : k->faint);
}

void PanelLookPage(const char *title, const PanelLookRow *rows, uint32_t count, uint32_t chosen,
                   const char *note, const char *hint)
{
    if (!g_look.room) return;
    const LookInks *k  = g_look.inks;
    const int32_t   W  = (int32_t)g_look.fb.width, H = (int32_t)g_look.fb.height;
    const int       wide = g_look.ly == 0;
    const int       over = !g_look.logo && g_look.said;

    /* The column's room cleared to the paper: from left of the plashka to the
     * screen's right edge, top to bottom — or under the logo, standing up. */
    const int32_t top = wide ? 0 : g_look.ly + g_look.lh;
    BoxBrushRect  room = { g_look.left, top, W - g_look.left, H - top };
    BoxBrushClear(&g_look.canvas, room, k->paper);

    /* Rows two cells tall and a cell between groups — closer, when the block
     * would reach down into the hints under it on a short screen. */
    int32_t rh = 2 * g_look.ch, gap = g_look.ch, block = 0, names = 0;
    for (int tight = 0; tight < 2; tight++) {
        block = (title ? 2 * g_look.ch : 0) + (over ? 4 * g_look.ch : 0);
        for (uint32_t i = 0; i < count; i++) block += rows[i].label ? rh : gap;
        if (block <= H - 5 * g_look.sh - top - g_look.ch) break;
        rh  = g_look.ch * 3 / 2;
        gap = g_look.ch / 2;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (!rows[i].info) continue;
        const int32_t a = Across(rows[i].label, g_look.size) + 2 * g_look.cw;
        if (a > names) names = a;
    }
    int32_t y = wide ? H / 2 - block / 2 : top + (H - top) / 2 - block / 2;
    if (y < top + g_look.ch) y = top + g_look.ch;
    if (over) {
        Say(g_look.size, g_look.mx, y, g_look.said, k->told);
        if (g_look.under) Say(g_look.small, g_look.mx, y + g_look.ch * 3 / 2, g_look.under, k->faint);
        y += 4 * g_look.ch;
    }

    if (title) {
        Say(g_look.size, g_look.mx, y, title, k->ink);
        y += 2 * g_look.ch;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (!rows[i].label) {
            Solid(g_look.mx, y + gap / 2 - 1, g_look.mw / 3, 2, 1, k->rule);
            y += gap;
            continue;
        }
        LookRow(&rows[i], i == chosen, y, rh, names);
        y += rh;
    }

    /* The hint under it all, a line at a time, and a note under the hint. */
    int32_t hy = H - 4 * g_look.sh;
    for (const char *line = hint; line && *line;) {
        char     one[160];
        uint32_t n = 0;
        while (line[n] && line[n] != '\n' && n < sizeof(one) - 1u) {
            one[n] = line[n];
            n++;
        }
        one[n] = '\0';
        Say(g_look.small, g_look.mx, hy, one, k->faint);
        hy += g_look.sh * 3 / 2;
        line += n;
        if (*line == '\n') line++;
    }
    if (note) Say(g_look.small, g_look.mx, hy, note, k->told);

    volatile uint8_t *glass = (volatile uint8_t *)(uintptr_t)g_look.fb.addr;
    if (!g_look.shown) {
        BoxBrushShow(&g_look.canvas, g_look.canvas.Clip, glass, g_look.fb.stride, g_look.fb.format);
        g_look.shown = 1;
    } else {
        BoxBrushShow(&g_look.canvas, room, glass, g_look.fb.stride, g_look.fb.format);
    }
    __asm__ volatile("sfence" ::: "memory");
}

void PanelLookEnd(void)
{
    if (g_look.room) g_bs->free_pages(g_look.room, g_look.pages);
    g_look.room  = 0;
    g_look.shown = 0;
}
