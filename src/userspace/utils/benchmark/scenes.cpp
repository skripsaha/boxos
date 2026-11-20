/* scenes.cpp — what `benchmark graphics` has the display show, and how
 * smoothly each goes. See benchmark.h.
 *
 * Each scene is a screenful this program changes every frame, as a program
 * does: a step said and presented, the next said once the display tells the
 * step shown. Half a second to settle — the scene composed whole the first
 * time — then five seconds measured, as a person sees them: how often a frame
 * came; how evenly — the time from a frame to the next, within which half of
 * them, 95 and 99 in a hundred came, and the worst; how many came late, past
 * a frame and a half of sixty a second; and what each cost the display as it
 * tells (Event::Micros, the frame composed and put on the glass; Pixels, those
 * it composed), and the processor time the display and this program took
 * meanwhile, in cores. A scene is smooth when 95 frames in a hundred come
 * within 20 ms and at least 55 a second; uneven while they come within 40;
 * past that a slideshow. */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include "benchmark.h"
#include "box/clock.h"
#include "box/system.h"
#include "painter/core/canvas.h"
#include "painter/core/color.h"
#include "painter/core/path.h"
#include "painter/gui/area.h"
#include "painter/gui/items.h"
#include "painter/tui/keyboard.h"

namespace benchmark {

namespace {

namespace gui = painter::gui;
using painter::core::Color;
using painter::core::Corners;
using painter::core::PointF;
using Clock = std::chrono::steady_clock;

constexpr std::uint64_t SettleUs = 500000;
constexpr std::uint64_t LengthUs = 5000000;
constexpr double        FrameMs  = 1000.0 / 60;
constexpr double        LateMs   = FrameMs * 1.5;
constexpr int           Scenes8  = 8;

/* The same numbers every run, so every run shows the same scenes. */
class Dice {
public:
    float Unit() noexcept
    {
        State ^= State << 13;
        State ^= State >> 17;
        State ^= State << 5;
        return static_cast<float>(State % 100000u) / 100000.0f;
    }
    float Between(float From, float To) noexcept { return From + (To - From) * Unit(); }

private:
    std::uint32_t State = 0x9E3779B9u;
};

/* A bright colour, Turn of the way round the hues. */
Color Hue(float Turn)
{
    const auto Part = [Turn](float Shift) {
        return static_cast<std::uint8_t>(127.5f + 127.5f * std::sin(6.2831853f * (Turn + Shift)));
    };
    return Color::Rgb(Part(0), Part(1.0f / 3), Part(2.0f / 3));
}

gui::Fill Across(Color From, Color To, PointF Size)
{
    gui::Fill Made;
    Made.What  = gui::Fill::Kind::Linear;
    Made.From  = {0, 0};
    Made.To    = Size;
    Made.Stops = {gui::Stop{0, From}, gui::Stop{1, To}};
    return Made;
}

/* A to and fro between 0 and Span, at Speed a second, from At. */
float Bounce(float At, float Speed, float Span, double Seconds)
{
    if (Span <= 0) return 0;
    const float Gone = std::fmod(At + Speed * static_cast<float>(Seconds), 2 * Span);
    return Gone < Span ? Gone : 2 * Span - Gone;
}

/* Whether Tags, joined by commas, holds Tag. */
bool Wears(const char *Tags, std::string_view Tag)
{
    for (std::string_view Rest = Tags ? Tags : ""; !Rest.empty();) {
        const std::size_t Comma = Rest.find(',');
        if (Rest.substr(0, Comma) == Tag) return true;
        if (Comma == std::string_view::npos) break;
        Rest.remove_prefix(Comma + 1);
    }
    return false;
}

/* The processor time the display's strands have had, in µs. */
std::uint64_t DisplayUs()
{
    RosterProgram *Raw   = nullptr;
    const int      Count = proc_roster(&Raw, nullptr);
    if (Count < 0) return 0;
    std::uint64_t Us = 0;
    for (int I = 0; I < Count; I++)
        if (Wears(Raw[I].tags, "display"))
            for (std::uint32_t S = 0; S < Raw[I].strand_count; S++) Us += Raw[I].strands[S].cpu_us;
    std::free(Raw);
    return Us;
}

std::uint64_t OwnUs()
{
    std::uint64_t Us = 0;
    return proc_cpu_time(&Us) == 0 ? Us : 0;
}

/* The share of the times, From 0 to 1, that come within the time returned. */
double Within(const std::vector<double> &Sorted, double Share)
{
    if (Sorted.empty()) return 0;
    const auto At = static_cast<std::size_t>(std::ceil(Share * static_cast<double>(Sorted.size())));
    return Sorted[std::min(Sorted.size() - 1, At == 0 ? 0 : At - 1)];
}

/* The screen the scenes are shown on, and how each went. */
class Stage {
public:
    Stage(gui::Area &On, painter::tui::Keyboard &Heard, Report &Said)
        : Screen(On), Size(On.Size()), Keys(Heard), Out(Said), Page(On.Rectangle(gui::Role::Page)),
          Caption(On.Text("", gui::Role::Hint))
    {
        Page.SetPlace({0, 0}).SetSize(Size);
        Caption.SetPlace({16, 10});
        Got.Count = Scenes8;
    }

    /* The scene Name, of What, played: Step says what changed by the time
     * given, in seconds from its beginning; its line said. Ahead, each step
     * is said while the one before it is composed — the glass awaited only up
     * to that one — as a program drawing its own pixels draws the next
     * picture on one page while the display shows the other. False once it
     * was stopped. */
    bool Play(const char *Name, const char *What, const std::function<void(double)> &Step, bool Ahead = false);

    /* What a canvas scene's own drawing took this frame, in µs: told by the
     * scene, said beside the display's. */
    void Drew(double Us) noexcept { Drawing += Us, Drawn++; }

    gui::Area   &Screen;
    const PointF Size;
    Dice         Roll;
    Tally        Got;

private:
    std::optional<gui::Event> ShownOf(std::uint64_t Step);

    painter::tui::Keyboard &Keys;
    Report                 &Out;
    gui::Shape              Page;
    gui::Text               Caption;
    int                     Played  = 0;
    double                  Drawing = 0;
    std::uint32_t           Drawn   = 0;
};

std::optional<gui::Event> Stage::ShownOf(std::uint64_t Step)
{
    for (;;) {
        const gui::Event Heard = Screen.Wait(&Keys);
        switch (Heard.What) {
        case gui::Event::Kind::Shown:
            if (Heard.Step >= Step) return Heard;
            break;
        case gui::Event::Kind::Refused: Got.Refused++; break;
        case gui::Event::Kind::Gone:
        case gui::Event::Kind::EndAsked: return std::nullopt;
        case gui::Event::Kind::Key:
            if (!Heard.Key.IsExtended() && Heard.Key.Character == painter::tui::KeyCodes::Escape) return std::nullopt;
            break;
        default: break;
        }
    }
}

bool Stage::Play(const char *Name, const char *What, const std::function<void(double)> &Step, bool Ahead)
{
    if (Got.Stopped) return false;
    char Title[160];
    std::snprintf(Title, sizeof Title, "benchmark graphics — %s, %d of %d — esc stops", Name, ++Played, Got.Count);
    Caption.SetText(Title);

    std::vector<double> Gaps, Costs;
    std::uint64_t       Pixels = 0, From = 0, Last = 0, DisplayFrom = 0, OwnFrom = 0, Saying = 0;
    const std::uint64_t Began  = clock_uptime_us();
    for (;;) {
        const std::uint64_t Before = Screen.Step();
        const std::uint64_t Asked  = clock_uptime_us();
        Step(static_cast<double>(Asked - Began) / 1e6);
        Screen.PresentAll();
        if (From != 0) Saying += clock_uptime_us() - Asked;
        if (Screen.Step() == Before) continue;   // nothing changed, nothing to show
        const std::optional<gui::Event> Shown = ShownOf(Ahead ? Before : Screen.Step());
        if (!Shown) {
            Got.Stopped = true;
            Out.Say("scene %s — stopped", Name);
            return false;
        }
        const std::uint64_t At = clock_uptime_us();
        if (From != 0) {
            Gaps.push_back(static_cast<double>(At - Last) / 1000.0);
            Costs.push_back(static_cast<double>(Shown->Micros) / 1000.0);
            Pixels += Shown->Pixels;
            if (At - From >= LengthUs) {
                Last = At;
                break;
            }
        } else if (At - Began >= SettleUs) {
            From        = At;
            DisplayFrom = DisplayUs();
            OwnFrom     = OwnUs();
            Drawing     = 0;
            Drawn       = 0;
        }
        Last = At;
    }

    const double Seconds = static_cast<double>(Last - From) / 1e6;
    const double Frames  = static_cast<double>(Gaps.size());
    std::vector<double> Sorted = Gaps;
    std::sort(Sorted.begin(), Sorted.end());
    const double Half = Within(Sorted, 0.5), Most95 = Within(Sorted, 0.95), Most99 = Within(Sorted, 0.99);
    const auto   Late = std::count_if(Gaps.begin(), Gaps.end(), [](double Gap) { return Gap > LateMs; });
    double       Cost = 0, CostMost = 0;
    for (double Each : Costs) Cost += Each, CostMost = std::max(CostMost, Each);
    const double PerSecond = Frames / Seconds;
    const char  *Verdict   = Most95 <= 20 && PerSecond >= 55 ? "smooth" : Most95 <= 40 ? "uneven" : "slideshow";
    (Most95 <= 20 && PerSecond >= 55 ? Got.Smooth : Most95 <= 40 ? Got.Uneven : Got.Slideshow)++;
    char Own[96] = "";
    if (Drawn != 0) std::snprintf(Own, sizeof Own, "; benchmark drew its pixels in %.2f ms a frame", Drawing / Drawn / 1000.0);
    Out.Say("scene %s — %s: %.1f frames a second; a frame every %.1f ms (95%% within %.1f, 99%% within %.1f, worst %.1f), %ld "
            "late; the display %.2f ms a frame (most %.2f), %.0f pixels composed a frame; benchmark %.2f ms a frame saying "
            "it%s; processor: the display %.2f cores, benchmark %.2f — %s",
            Name, What, PerSecond, Half, Most95, Most99, Sorted.back(), static_cast<long>(Late), Cost / Frames, CostMost,
            static_cast<double>(Pixels) / Frames, static_cast<double>(Saying) / 1000.0 / Frames, Own, static_cast<double>(DisplayUs() - DisplayFrom) / 1e6 / Seconds,
            static_cast<double>(OwnUs() - OwnFrom) / 1e6 / Seconds, Verdict);
    return true;
}

/* A panel of a gradient with rounded corners and a soft shadow. */
gui::Shape Panel(gui::Area &On, PointF At, PointF Size, float Turn)
{
    gui::Shape Made = On.Rectangle();
    Made.SetPlace(At).SetSize(Size).SetCorners(Corners::All(18));
    Made.SetFill(Across(Hue(Turn), Hue(Turn + 0.3f), Size));
    Made.SetShadow(gui::Shadow{{0, 10}, 28, 0, Color::Rgba(0, 0, 0, 120), false});
    return Made;
}

/* One small dot moving over a still screen of heavy things: a frame should
 * cost the display the dot's pixels, not the screen's. */
void Quiet(Stage &On)
{
    const float  Gap = 32, Wide = (On.Size.X - 5 * Gap) / 4, High = (On.Size.Y - 4 * Gap - 40) / 3;
    std::vector<gui::Shape> Plates;
    std::vector<gui::Text>  Labels;
    for (int I = 0; I < 12; I++) {
        const PointF At{Gap + static_cast<float>(I % 4) * (Wide + Gap), 40 + Gap + static_cast<float>(I / 4) * (High + Gap)};
        Plates.push_back(Panel(On.Screen, At, {Wide, High}, static_cast<float>(I) / 12));
        Labels.push_back(On.Screen.Text("still, as the screen mostly is", gui::Role::Body));
        Labels.back().SetPlace({At.X + 20, At.Y + 20});
    }
    gui::Shape Dot = On.Screen.Ellipse();
    Dot.SetSize({14, 14}).SetFill(gui::Fill::Of(Color::Rgb(255, 255, 255)));
    On.Play("quiet", "12 still panels of gradients, shadows and words, one small dot moving over them", [&](double T) {
        Dot.SetPlace({On.Size.X / 2 + std::cos(static_cast<float>(T) * 2) * On.Size.X * 0.4f,
                      On.Size.Y / 2 + std::sin(static_cast<float>(T) * 2) * On.Size.Y * 0.4f});
    });
}

/* Many small opaque shapes, every one moved each frame: the simplest work. */
void Shapes(Stage &On)
{
    struct Moving {
        gui::Shape Plate;
        float      X, Y, Vx, Vy;
    };
    std::vector<Moving> All;
    for (int I = 0; I < 400; I++) {
        Moving One{On.Screen.Rectangle(), On.Roll.Between(0, On.Size.X), On.Roll.Between(0, On.Size.Y), On.Roll.Between(-300, 300),
                   On.Roll.Between(-300, 300)};
        One.Plate.SetSize({18, 12}).SetFill(gui::Fill::Of(Hue(On.Roll.Unit())));
        All.push_back(std::move(One));
    }
    On.Play("shapes", "400 small opaque rectangles, every one moved each frame", [&](double T) {
        for (Moving &One : All)
            One.Plate.SetPlace({Bounce(One.X, One.Vx, On.Size.X - 18, T), Bounce(One.Y, One.Vy, On.Size.Y - 12, T)});
    });
}

/* Heavy things moving: big panels of gradients, rounded and shadowed. */
void Panels(Stage &On)
{
    struct Moving {
        gui::Shape Plate;
        float      X, Y, Vx, Vy;
    };
    const PointF        Size{260, 160};
    std::vector<Moving> All;
    for (int I = 0; I < 24; I++) {
        Moving One{Panel(On.Screen, {0, 0}, Size, static_cast<float>(I) / 24), On.Roll.Between(0, On.Size.X - Size.X),
                   On.Roll.Between(0, On.Size.Y - Size.Y), On.Roll.Between(-220, 220), On.Roll.Between(-160, 160)};
        All.push_back(std::move(One));
    }
    On.Play("panels", "24 big panels of gradients, rounded and shadowed, every one moved each frame", [&](double T) {
        for (Moving &One : All)
            One.Plate.SetPlace({Bounce(One.X, One.Vx, On.Size.X - Size.X, T), Bounce(One.Y, One.Vy, On.Size.Y - Size.Y, T)});
    });
}

/* Words changing every frame: text set and drawn anew. */
void Lettering(Stage &On)
{
    std::vector<gui::Text> Lines;
    const int              Count = std::max(8, static_cast<int>((On.Size.Y - 60) / 26));
    for (int I = 0; I < Count; I++) {
        Lines.push_back(On.Screen.Text("", gui::Role::Body));
        Lines.back().SetPlace({40, 50 + static_cast<float>(I) * 26});
    }
    std::uint32_t Frame = 0;
    On.Play("text", "a screen of lines, every one set anew each frame", [&](double T) {
        Frame++;
        char Line[160];
        for (std::size_t I = 0; I < Lines.size(); I++) {
            std::snprintf(Line, sizeof Line, "line %zu of a page changing as it is read — frame %u, %.3f seconds in, %s", I + 1, Frame,
                          T, (Frame + I) % 2 ? "kerning AVATAR, Wa, To" : "\xD1\x82\xD0\xB5\xD0\xBA\xD1\x81\xD1\x82 \xE2\x80\x94 text");
            Lines[I].SetText(Line);
        }
    });
}

/* Groups seen through: each a panel and its words faded in and out. */
void Fades(Stage &On)
{
    struct Fading {
        gui::Group Whole;
        gui::Shape Plate;
        gui::Text  Said;
    };
    const PointF        Size{340, 200};
    std::vector<Fading> All;
    for (int I = 0; I < 12; I++) {
        Fading One{On.Screen.Group(), {}, {}};
        One.Whole.SetPlace({40 + static_cast<float>(I % 4) * (On.Size.X - 80 - Size.X) / 3,
                            60 + static_cast<float>(I / 4) * (On.Size.Y - 100 - Size.Y) / 2});
        One.Plate = One.Whole.Rectangle();
        One.Plate.SetSize(Size).SetCorners(Corners::All(18)).SetFill(Across(Hue(static_cast<float>(I) / 12), Hue(0.5f), Size));
        One.Said = One.Whole.Text("seen through, fading", gui::Role::Heading);
        One.Said.SetPlace({20, 20});
        All.push_back(std::move(One));
    }
    On.Play("fades", "12 groups of a panel and words, each faded in and out", [&](double T) {
        for (std::size_t I = 0; I < All.size(); I++)
            All[I].Whole.SetOpacity(0.2f + 0.8f * (0.5f + 0.5f * std::sin(static_cast<float>(T) * 3 + static_cast<float>(I))));
    });
}

/* Hundreds of small things scaled and turned at once, as a pond of tiles
 * struck by many stones. */
void Tiles(Stage &On)
{
    std::vector<gui::Shape> All;
    const int Across = static_cast<int>((On.Size.X - 40) / 40), Down = static_cast<int>((On.Size.Y - 60) / 40);
    for (int Y = 0; Y < Down; Y++)
        for (int X = 0; X < Across && All.size() < 600; X++) {
            All.push_back(On.Screen.Rectangle());
            All.back()
                .SetPlace({20 + static_cast<float>(X) * 40, 50 + static_cast<float>(Y) * 40})
                .SetSize({30, 30})
                .SetCorners(Corners::All(7))
                .SetFill(gui::Fill::Of(Hue(static_cast<float>(X + Y) / 40)));
        }
    On.Play("tiles", "hundreds of small tiles, every one scaled and turned each frame", [&](double T) {
        for (std::size_t I = 0; I < All.size(); I++) {
            const float Wave = std::sin(static_cast<float>(T) * 4 - static_cast<float>(I % 37) * 0.3f);
            All[I].SetScale({0.8f + 0.3f * Wave, 0.8f + 0.3f * Wave}).SetTurn(Wave * 30);
        }
    });
}

/* A program's own pixels, every one drawn anew each frame and shown: as a
 * fractal, a game, a film is. */
void DrawnBy(Stage &On, gui::Canvas &Board, std::uint32_t Wide, std::uint32_t High, double T)
{
    (void)Board.Draw([&](painter::core::Canvas &Page) {
        const Clock::time_point Began = Clock::now();
        Page.Clear(Hue(static_cast<float>(T) * 0.1f));
        for (int I = 0; I < 24; I++) {
            const float X = Bounce(static_cast<float>(I) * 97, 180 + static_cast<float>(I) * 11, static_cast<float>(Wide) - 40, T);
            (void)Page.Fill(painter::core::Path::Rect({X, 0, 40, static_cast<float>(High)}), Hue(static_cast<float>(I) / 24));
        }
        On.Drew(std::chrono::duration<double, std::micro>(Clock::now() - Began).count());
    });
}

void Pixels(Stage &On)
{
    const float         Scale = On.Screen.Scale();
    const std::uint32_t Wide  = static_cast<std::uint32_t>(On.Size.X * Scale), High = static_cast<std::uint32_t>(On.Size.Y * Scale);
    gui::Canvas         Board = On.Screen.Canvas({static_cast<std::int32_t>(Wide), static_cast<std::int32_t>(High)});
    Board.SetPlace({0, 0});
    On.Play("canvas", "the whole screen of a program's own pixels, drawn anew each frame", [&](double T) { DrawnBy(On, Board, Wide, High, T); },
            true);
}

/* Everything at once: what a busy screen is — heavy panels moving, a pond of
 * tiles, words changing, groups fading and a program's own pixels. */
void Everything(Stage &On)
{
    std::vector<gui::Shape> Moving, Pond, Plates;
    std::vector<gui::Text>  Lines;
    std::vector<gui::Group> Seen;
    const PointF            Size{240, 150};
    for (int I = 0; I < 12; I++) Moving.push_back(Panel(On.Screen, {0, 0}, Size, static_cast<float>(I) / 12));
    for (int I = 0; I < 200; I++) {
        Pond.push_back(On.Screen.Rectangle());
        Pond.back()
            .SetPlace({On.Size.X - 20 * 36 + static_cast<float>(I % 20) * 36, On.Size.Y - 10 * 36 + static_cast<float>(I / 20) * 36})
            .SetSize({28, 28})
            .SetCorners(Corners::All(6))
            .SetFill(gui::Fill::Of(Hue(static_cast<float>(I) / 200)));
    }
    for (int I = 0; I < 4; I++) {
        Seen.push_back(On.Screen.Group());
        Seen.back().SetPlace({40 + static_cast<float>(I) * 260, 50});
        Plates.push_back(Seen.back().Rectangle());
        Plates.back().SetSize({220, 120}).SetCorners(Corners::All(14)).SetFill(gui::Fill::Of(Hue(static_cast<float>(I) / 4 + 0.5f)));
    }
    const std::uint32_t Wide = 640, High = 360;
    gui::Canvas         Board = On.Screen.Canvas({static_cast<std::int32_t>(Wide), static_cast<std::int32_t>(High)});
    Board.SetPlace({40, On.Size.Y - High / On.Screen.Scale() - 40});
    for (int I = 0; I < 12; I++) {
        Lines.push_back(On.Screen.Text("", gui::Role::Body));
        Lines.back().SetPlace({On.Size.X - 560, 200 + static_cast<float>(I) * 24});
    }
    std::uint32_t Frame = 0;
    On.Play("everything", "panels moving, a pond of tiles, words changing, groups fading and a canvas, at once", [&](double T) {
        Frame++;
        for (std::size_t I = 0; I < Moving.size(); I++)
            Moving[I].SetPlace({Bounce(static_cast<float>(I) * 131, 150 + static_cast<float>(I) * 9, On.Size.X - Size.X, T),
                                Bounce(static_cast<float>(I) * 71, 110 + static_cast<float>(I) * 7, On.Size.Y - Size.Y, T)});
        for (std::size_t I = 0; I < Pond.size(); I++) {
            const float Wave = std::sin(static_cast<float>(T) * 4 - static_cast<float>(I % 23) * 0.3f);
            Pond[I].SetScale({0.8f + 0.3f * Wave, 0.8f + 0.3f * Wave}).SetTurn(Wave * 30);
        }
        for (std::size_t I = 0; I < Seen.size(); I++)
            Seen[I].SetOpacity(0.2f + 0.8f * (0.5f + 0.5f * std::sin(static_cast<float>(T) * 3 + static_cast<float>(I))));
        char Line[96];
        for (std::size_t I = 0; I < Lines.size(); I++) {
            std::snprintf(Line, sizeof Line, "busy line %zu — frame %u", I + 1, Frame);
            Lines[I].SetText(Line);
        }
        DrawnBy(On, Board, Wide, High, T);
    });
}

}  // namespace

Tally Scenes(gui::Area &On, painter::tui::Keyboard &Keys, Report &Out)
{
    Stage Shown(On, Keys, Out);
    Quiet(Shown);
    Shapes(Shown);
    Panels(Shown);
    Lettering(Shown);
    Fades(Shown);
    Tiles(Shown);
    Pixels(Shown);
    Everything(Shown);
    return Shown.Got;
}

}  // namespace benchmark
