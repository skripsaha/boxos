/*
 * scene.cpp — the scene the loader left on the glass, played on. See scene.h.
 *
 * Its file is read by the fid the claim names — the one the loader read by its
 * name, boot.scene — and held to its bounds; its first frame is made on a
 * canvas as big as the glass, the very pixels the loader drew, and shown over
 * them once: nothing on the glass moves, and nothing that might differ stays.
 * From then on time moves it — a frame every FrameMs, by the alarm the event
 * loop sets for SceneDue — and each frame is drawn only where it changed.
 *
 * Only its beginning is said, before any program has a prompt on the screen.
 * Its going is not: a line said then would stand on the wire between a
 * program's prompt and what the person types at it. The going shows on the
 * glass, and what took it away — a key — is in the log already.
 */

#include "scene.h"

#include <cstddef>
#include <memory>
#include <new>
#include <vector>

#include "box/clock.h"
#include "box/debug.h"
#include "box/file.h"
#include "boxos_scene_play.h"

namespace {

// About thirty frames a second, as an effect over the glass moves.
constexpr std::uint64_t FrameMs = 33;

// The most dirty rectangles a frame is drawn in.
constexpr std::uint32_t DirtyMost = 8;

struct Playing {
    std::vector<std::uint8_t>  Bytes;
    BoxScene                   Scene{};
    std::vector<std::uint32_t> Pixels;
    std::vector<std::int32_t>  Row;
    std::vector<BoxScenePose>  Start, Now;
    std::vector<BoxSceneLaid>  Laid, Shown;
    std::vector<BoxBrushEdge>  Edges;
    BoxBrushCanvas             Canvas{};
    BoxScenePlayer             Player{};
    std::uint64_t              ActBegan = 0;   // uptime ms the act began at
    std::uint64_t              Due = 0;        // its next frame
    bool                       Ready = false;  // the machine has waited for the person
};

std::unique_ptr<Playing> g_play;
Easel                   *g_on = nullptr;

// The file `Fid` whole, at most BOX_SCENE_FILE_MOST bytes: why not, or NULL.
const char *ReadScene(file_id_t Fid, std::vector<std::uint8_t> &Bytes)
{
    FileMetadata Metadata;
    if (FileMetadataRead(Fid, &Metadata) != 0) return "its file could not be described";
    const std::uint64_t Size = Metadata.Size;
    FileMetadataRelease(&Metadata);
    if (Size > BOX_SCENE_FILE_MOST) return "its file is bigger than a scene is read";
    Bytes.resize(Size);
    std::uint64_t At = 0;
    while (At < Size) {
        const std::int64_t Got = fread(Fid, At, Bytes.data() + At, Size - At);
        if (Got <= 0) return "its file would not read to its end";
        At += static_cast<std::uint64_t>(Got);
    }
    return nullptr;
}

// The frame Ms into the act drawn where it changed, onto the glass.
void Draw(Playing &P, std::uint32_t Ms)
{
    BoxBrushRect   Dirty[DirtyMost];
    const uint32_t Count = BoxScenePlayAt(&P.Player, Ms, Dirty, DirtyMost);
    BoxScenePlayDraw(&P.Player, Dirty, Count);
    g_on->Show(P.Canvas, Dirty, Count);
}

// The act begun, from where every figure stands At ms into the one before.
void Act(Playing &P, std::uint32_t At, std::uint32_t Next, std::uint64_t Began)
{
    BoxBrushRect Ignored[DirtyMost];
    (void)BoxScenePlayAt(&P.Player, At, Ignored, DirtyMost);
    BoxScenePlayAct(&P.Player, Next);
    P.ActBegan = Began;
}

void Gone()
{
    g_play.reset();
}

}  // namespace

bool SceneBegin(Easel &On) noexcept
{
    if (!On.Pixels() || On.Left() != SCREEN_LEFT_SCENE) return false;
    const file_id_t Fid = On.LeftScene();
    try {
        auto        P   = std::make_unique<Playing>();
        const char *Why = ReadScene(Fid, P->Bytes);
        if (!Why) Why = BoxSceneRead(P->Bytes.data(), P->Bytes.size(), &P->Scene);
        if (Why) {
            kdbg_print("[display] scene: the one the loader left (fid %lu) will not play — %s; the console is drawn",
                       static_cast<unsigned long>(Fid), Why);
            return false;
        }
        const std::uint32_t W = On.PixelWidth(), H = On.PixelHeight();
        const std::uint32_t Figures = P->Scene.Head->Figures;
        P->Pixels.resize(static_cast<std::size_t>(W) * H);
        P->Row.resize(static_cast<std::size_t>(W) + 2u);
        P->Start.resize(Figures);
        P->Now.resize(Figures);
        P->Laid.resize(Figures);
        P->Shown.resize(Figures);
        P->Edges.resize(BOX_SCENE_EDGES);
        P->Canvas.Pixels      = P->Pixels.data();
        P->Canvas.Width       = W;
        P->Canvas.Height      = H;
        P->Canvas.Stride      = W;
        P->Canvas.Clip.Width  = static_cast<std::int32_t>(W);
        P->Canvas.Clip.Height = static_cast<std::int32_t>(H);
        P->Canvas.Row         = P->Row.data();
        BoxScenePlayBegin(&P->Player, &P->Scene, &P->Canvas, P->Start.data(), P->Now.data(), P->Laid.data(),
                          P->Shown.data(), P->Edges.data());
        g_on = &On;
        P->ActBegan = clock_uptime_ms();
        P->Due      = P->ActBegan + FrameMs;
        Draw(*P, 0);
        kdbg_print("[display] scene: the one the loader left (fid %lu, %u figures) goes on from its frame, %ux%u",
                   static_cast<unsigned long>(Fid), Figures, W, H);
        g_play = std::move(P);
        return true;
    } catch (const std::bad_alloc &) {
        kdbg_print("[display] scene: no room to play the one the loader left; the console is drawn");
        return false;
    }
}

bool SceneOn() noexcept
{
    return g_play != nullptr;
}

bool SceneStep(bool Ready) noexcept
{
    if (!g_play) return false;
    Playing            &P    = *g_play;
    const BoxSceneHead *Head = P.Scene.Head;
    const std::uint64_t Now  = clock_uptime_ms();
    bool                Moved = false;

    /* Enter is played to its end whatever comes; the machine ready by then,
     * leave follows it — and wait, once begun, leaves from where it stands. */
    P.Ready |= Ready;
    if (P.Ready && P.Player.Act == BOX_SCENE_WAIT) {
        Act(P, static_cast<std::uint32_t>(Now - P.ActBegan), BOX_SCENE_LEAVE, Now);
        Moved = true;
    }
    if (Now < P.Due) return Moved;

    std::uint64_t Ms = Now - P.ActBegan;
    if (P.Player.Act == BOX_SCENE_ENTER && Ms >= Head->Acts[BOX_SCENE_ENTER]) {
        Act(P, Head->Acts[BOX_SCENE_ENTER], P.Ready ? BOX_SCENE_LEAVE : BOX_SCENE_WAIT,
            P.ActBegan + Head->Acts[BOX_SCENE_ENTER]);
        Ms -= Head->Acts[BOX_SCENE_ENTER];
    }
    if (P.Player.Act == BOX_SCENE_WAIT) Ms %= Head->Acts[BOX_SCENE_WAIT];
    const bool Over = P.Player.Act == BOX_SCENE_LEAVE && Ms >= Head->Acts[BOX_SCENE_LEAVE];
    Draw(P, static_cast<std::uint32_t>(Over ? Head->Acts[BOX_SCENE_LEAVE] : Ms));
    if (Over) {
        Gone();
        return true;
    }
    P.Due = Now + FrameMs;
    return true;
}

void SceneEnd() noexcept
{
    Gone();
}

std::uint64_t SceneDue() noexcept
{
    return g_play ? g_play->Due : 0;
}
