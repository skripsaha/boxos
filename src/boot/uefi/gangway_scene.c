/*
 * Gangway — the scene the machine starts with (boxos_scene.h): boot.scene,
 * read beside the kernel (gangway_attest.c) and held to its every bound at
 * once (SceneTake), its first frame drawn on the screen the kernel is handed
 * once the panel, if any, is left (SceneShow) — one frame, and nothing waited
 * for: the display goes on from that very frame (boxos_scene_play.h), told by
 * a stamp of the Boarding Pass which file it is (SceneStamp,
 * gangway_handoff.c).
 *
 * The frame is made in memory and shown whole, so the screen goes from the
 * firmware's picture to the scene's in one stroke. It is not drawn — and why
 * is said — when the person turned the scene off (gangway.settings, or the
 * panel for one start), when the start shows all it says from its first line
 * (startup all: the kernel's screen does that), with no framebuffer, with no
 * boot.scene or one that is no scene, or without the room to draw it. Its
 * pages are given back before the memory map is built, drawn or not.
 */

#include "gangway.h"
#include "boxos_scene_play.h"

typedef struct {
    uint8_t *bytes;   /* the file, in the Lectern's room; NULL when there is none to draw */
    uint64_t room;
    uint64_t size;
    uint64_t fid;
    BoxScene scene;
    int      shown;   /* its first frame is on the screen */
} SceneKept;

static SceneKept g_scene;

static UINTN ScenePages(uint64_t bytes)
{
    return (UINTN)((bytes + 4095u) / 4096u);
}

static void SceneGiveBack(void)
{
    if (g_scene.bytes) g_bs->free_pages((EFI_PHYSICAL_ADDRESS)(uintptr_t)g_scene.bytes, ScenePages(g_scene.room));
    g_scene.bytes = NULL;
}

void SceneTake(const LecternCalled *called)
{
    MemZero(&g_scene, sizeof(g_scene));
    if (called->says == CALLED_NONE) return;   /* the Lectern said why */
    if (!called->bytes) {
        Print("Gangway: " BOX_SCENE_BOOT " is empty — no scene is drawn\n");
        return;
    }
    g_scene.bytes = called->bytes;
    g_scene.room  = called->room;
    g_scene.size  = called->size;
    g_scene.fid   = called->fid;

    const char *why = BoxSceneRead(g_scene.bytes, g_scene.size, &g_scene.scene);
    if (why) {
        Print("Gangway: " BOX_SCENE_BOOT " is no scene to draw — ");
        Print(why);
        Print(" (its record ");
        PrintDec(g_scene.scene.Fault);
        Print(")\n");
        SceneGiveBack();
        return;
    }
    const BoxSceneHead *h = g_scene.scene.Head;
    Print("Gangway: " BOX_SCENE_BOOT " is a scene of ");
    PrintDec(h->Figures);
    Print(" figure(s), its acts ");
    PrintDec(h->Acts[BOX_SCENE_ENTER]);
    Print(" ms in, ");
    PrintDec(h->Acts[BOX_SCENE_WAIT]);
    Print(" round while the machine gets ready, ");
    PrintDec(h->Acts[BOX_SCENE_LEAVE]);
    Print(" out\n");
}

/* Why the scene is not drawn on `fb`, or NULL. */
static const char *SceneOff(const FbInfo *fb)
{
    if (!g_settings.scene) return "the scene is off";
    if (g_settings.startup == BOARDING_STARTUP_ALL) return "the start shows all it says from its first line";
    if (!fb->addr || !fb->width || !fb->height) return "there is no framebuffer to draw it on";
    return NULL;
}

/* `bytes` carved off `*at`, 16 to a step. */
static uint8_t *Carve(uint8_t **at, uint64_t bytes)
{
    uint8_t *got = *at;
    *at += (bytes + 15u) & ~15ull;
    return got;
}

void SceneShow(const FbInfo *fb)
{
    if (!g_scene.bytes) return;
    const char *off = SceneOff(fb);
    if (off) {
        Print("Gangway: no scene is drawn — ");
        Print(off);
        Print("\n");
        SceneGiveBack();
        return;
    }

    const uint32_t       figures = g_scene.scene.Head->Figures;
    const uint64_t       pixels  = (uint64_t)fb->width * fb->height * 4u;
    const uint64_t       row     = ((uint64_t)fb->width + 2u) * 4u;
    const uint64_t       poses   = (uint64_t)figures * sizeof(BoxScenePose);
    const uint64_t       lays    = (uint64_t)figures * sizeof(BoxSceneLaid);
    const uint64_t       edges   = (uint64_t)BOX_SCENE_EDGES * sizeof(BoxBrushEdge);
    const uint64_t       bytes   = pixels + row + 2u * poses + 2u * lays + edges + 6u * 16u;
    EFI_PHYSICAL_ADDRESS room    = 0;
    if (EFI_ERROR(g_bs->allocate_pages(AllocateAnyPages, EfiLoaderData, ScenePages(bytes), &room))) {
        Print("Gangway: no scene is drawn — the loader has no room to draw it\n");
        SceneGiveBack();
        return;
    }

    uint8_t       *at = (uint8_t *)(uintptr_t)room;
    BoxBrushCanvas canvas;
    canvas.Pixels      = (uint32_t *)(void *)Carve(&at, pixels);
    canvas.Row         = (int32_t *)(void *)Carve(&at, row);
    canvas.Width       = fb->width;
    canvas.Height      = fb->height;
    canvas.Stride      = fb->width;
    canvas.Clip.X      = 0;
    canvas.Clip.Y      = 0;
    canvas.Clip.Width  = (int32_t)fb->width;
    canvas.Clip.Height = (int32_t)fb->height;

    BoxScenePose  *start = (BoxScenePose *)(void *)Carve(&at, poses);
    BoxScenePose  *now   = (BoxScenePose *)(void *)Carve(&at, poses);
    BoxSceneLaid  *laid  = (BoxSceneLaid *)(void *)Carve(&at, lays);
    BoxSceneLaid  *shown = (BoxSceneLaid *)(void *)Carve(&at, lays);
    BoxBrushEdge  *edge  = (BoxBrushEdge *)(void *)Carve(&at, edges);
    BoxScenePlayer player;
    BoxBrushRect   whole;
    BoxScenePlayBegin(&player, &g_scene.scene, &canvas, start, now, laid, shown, edge);
    BoxScenePlayDraw(&player, &whole, BoxScenePlayAt(&player, 0, &whole, 1));
    BoxBrushShow(&canvas, whole, (volatile uint8_t *)(uintptr_t)fb->addr, fb->stride, fb->format);

    g_bs->free_pages(room, ScenePages(bytes));
    SceneGiveBack();
    g_scene.shown = 1;
    Print("Gangway: the scene's first frame is on the screen, ");
    PrintDec(fb->width);
    Print("x");
    PrintDec(fb->height);
    Print(" — the display goes on from it\n");
    Moment("scene");
}

int SceneStamp(BoardingScene *out)
{
    if (!g_scene.shown) return 0;
    out->fid   = g_scene.fid;
    out->bytes = g_scene.size;
    return 1;
}
