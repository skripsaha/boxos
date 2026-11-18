/* itemcheck — the items' language held on the machine (box/items.h,
 * painter/gui/area.h): an area opened over the screen, a scene of every kind
 * of item said in one step — a page, a panel that clips, a heading, a text
 * sized by its content with a coloured stretch, a gradient, a group seen
 * through, a dashed outline, a picture of the program's own pixels, a clock,
 * cells in the console's look — and then runs of steps as a program makes
 * them: a word changed, a shape moved, a group faded, a cell changed. Each
 * step's frame is told back with what it cost: how long the display took to
 * compose it and put it on the glass, the pixels it put there, the ring's
 * bytes and records it took; and how long from the step to its frame told
 * back. Then painter::gui as a program writes with it (gui.cpp): objects,
 * a frame of a list, a canvas, a text measured beforehand, the C face. Then
 * the console: lines printed, an area opened in its flow after them, and how
 * long until the glass showed it all. The last line says PASS when every step
 * was shown, nothing was refused, a word and a cell changed each composed a
 * small part of the area, and the programmer's checks held; FAIL when not.
 * The times are a guide only under TCG. */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "box/form.h"
#include "box/touch.h"

#include "itemcheck_gui.h"
#include "painter/gui/area.h"

BOX_FORM("");

namespace {

using painter::core::Color;
using painter::core::PointF;
using painter::gui::Area;
using painter::gui::Event;
using painter::gui::Fill;
using painter::gui::Kind;
using painter::gui::Role;

constexpr float         Wide = 640, High = 400;   // the area wished, in points
constexpr std::uint32_t Steps = 60;               // each run's
constexpr std::uint32_t Page = 1, Panel = 10, Card = 2, Heading = 3, Body = 4, Bubble = 5, Glass = 6, Star = 9,
                        Checks = 11, Clock = 13, Cells = 14;
constexpr std::uint32_t CellsWide = 24, CellsHigh = 4;   // the cells' grid
constexpr std::uint32_t Lines = 400;                     // the console's lines printed

/* What the display told while the steps were waited for. */
struct Told {
    std::uint32_t Refused = 0;
    bool          Lost    = false;   // the bell would not ring
    bool          Same    = false;   // a step said only what was said: nothing went
};

/* The display's events until the frame showing Step is told: that frame's
 * Shown, every refusal counted and the first said. None when the area has no
 * bell to wait on. */
std::optional<Event> ShownTo(Area &On, std::uint64_t Step, Told &Heard)
{
    for (;;) {
        while (std::optional<Event> Each = On.Next()) {
            if (Each->What == Event::Kind::Refused && Heard.Refused++ == 0)
                std::printf("itemcheck: the display refused item %u's change %u of property %u — %d\n", Each->Item, Each->Op,
                            Each->Property, static_cast<int>(Each->Reason));
            if (Each->What == Event::Kind::Shown && Each->Step >= Step) return Each;
        }
        if (On.Bell() == TOUCH_TAG_INVALID) {
            Heard.Lost = true;
            return std::nullopt;
        }
        Touch Rang;
        (void)touch_wait_tag(On.Bell(), &Rang, 0);
    }
}

/* A picture of the program's own pixels: a checkerboard of colours. */
std::optional<std::uint32_t> Checkerboard(Area &On)
{
    auto Made = On.OpenPicture(32, 32);
    if (!Made) return std::nullopt;
    std::span<std::uint8_t> Pixels = Made->Pixels();
    for (std::uint32_t Y = 0; Y < 32; Y++)
        for (std::uint32_t X = 0; X < 32; X++) {
            std::uint8_t *P = &Pixels[(Y * 32 + X) * 4];
            const bool    On2 = ((X / 4) + (Y / 4)) % 2;
            P[0] = On2 ? 250 : 40, P[1] = static_cast<std::uint8_t>(X * 8), P[2] = static_cast<std::uint8_t>(Y * 8), P[3] = 255;
        }
    On.BlockChanged(Made->Id, painter::core::Rect{0, 0, 32, 32});
    return Made->Id;
}

/* The scene, said for an area of W x H points. */
void Scene(Area &Say, float W, float H, std::optional<std::uint32_t> Picture)
{
    Say.Create(Page, Kind::Shape, ITEM_AREA, ITEM_TOP, Role::Page);
    Say.Size(Page, {W, H});
    Say.Create(Panel, Kind::Group);
    Say.Place(Panel, {16, 16});
    Say.Size(Panel, {W * 0.55f, H * 0.6f});
    Say.Corners(Panel, painter::core::Corners::All(14));
    Say.Clip(Panel, true);
    Say.Create(Card, Kind::Shape, Panel, ITEM_TOP, Role::Panel);
    Say.Size(Card, {W * 0.55f, H * 0.6f});
    Say.Create(Heading, Kind::Text, Panel, ITEM_TOP, Role::Heading);
    Say.Place(Heading, {14, 10});
    Say.Text(Heading, "Items, on the machine");
    Say.Create(Body, Kind::Text, Panel, ITEM_TOP, Role::Body);
    Say.Place(Body, {14, 44});
    Say.Size(Body, {W * 0.5f, painter::gui::ByContent});
    Say.Text(Body, "What a program says is kept by the display and composed only where it changed \xE2\x80\x94 "
                   "\xD1\x82\xD0\xB5\xD0\xBA\xD1\x81\xD1\x82 of any size, in any font.");
    painter::gui::TextStyle Bold;
    Bold.Weight = 700;
    Bold.Color  = Color::Rgb(255, 120, 80);
    const painter::gui::Span Colored{7, 14, Bold};
    Say.Spans(Body, std::span(&Colored, 1));
    Say.Create(Bubble, Kind::Shape);
    Say.Ellipse(Bubble);
    Say.Place(Bubble, {W * 0.62f, 24});
    Say.Size(Bubble, {90, 70});
    Fill Round;
    Round.What   = Fill::Kind::Radial;
    Round.From   = {45, 35};
    Round.Radius = 50;
    Round.Focus  = {35, 25};
    Round.Stops  = {{0, Color::Rgb(255, 240, 200)}, {1, Color::Rgb(120, 60, 180)}};
    Say.Fills(Bubble, std::span(&Round, 1));
    Say.Create(Glass, Kind::Group);
    Say.Place(Glass, {W * 0.62f, H * 0.42f});
    Say.Opacity(Glass, 0.6f);
    Say.Create(7, Kind::Shape, Glass);
    Say.Size(7, {70, 50});
    const Fill Blue = Fill::Of(Color::Rgb(60, 140, 255));
    Say.Fills(7, std::span(&Blue, 1));
    Say.Create(8, Kind::Shape, Glass);
    Say.Place(8, {35, 25});
    Say.Size(8, {70, 50});
    const Fill Gold = Fill::Of(Color::Rgb(255, 200, 60));
    Say.Fills(8, std::span(&Gold, 1));
    Say.Corners(8, painter::core::Corners::All(10));
    Say.Create(Star, Kind::Shape);
    Say.Place(Star, {W * 0.1f, H * 0.7f});
    Say.Outline(Star, *painter::core::Path::FromSvg("M 30 0 L 37 22 L 60 22 L 41 36 L 48 58 L 30 44 L 12 58 L 19 36 L 0 22 L 23 22 Z"));
    painter::gui::Stroke Line;
    Line.Width  = 2.5f;
    Line.Dashes = {5, 3};
    Line.Join   = ITEM_JOIN_ROUND;
    Line.With   = Fill::OfTheme(painter::gui::ThemeColor::Accent);
    Say.Stroke(Star, Line);
    Say.Size(Star, {60, 60});
    if (Picture) {
        Say.Create(Checks, Kind::Picture);
        Say.Place(Checks, {W * 0.3f, H * 0.7f});
        Say.Size(Checks, {64, 48});
        Say.Picture(Checks, *Picture, ITEM_FIT_CONTAIN, true);
    }
    Say.Create(Clock, Kind::Text, ITEM_AREA, ITEM_TOP, Role::Label);
    Say.Place(Clock, {W * 0.55f, H * 0.82f});
    Say.Text(Clock, "12:00");
    /* Cells in the console's look, a caret in them. */
    Say.Create(Cells, Kind::Cells, ITEM_AREA, ITEM_TOP, Role::Code);
    Say.Place(Cells, {W * 0.05f, H * 0.80f});
    Say.Grid(Cells, CellsWide, CellsHigh);
    for (std::uint32_t Row = 0; Row < CellsHigh; Row++) {
        ItemCell Line[CellsWide];
        for (std::uint32_t Col = 0; Col < CellsWide; Col++)
            Line[Col] = ItemCell{static_cast<std::uint32_t>(U'a' + (Row * CellsWide + Col) % 26), 0x00E0E0E0u,
                                 Row % 2 ? 0x00203040u : COLOR_THROUGH, 0, ITEM_MARK_NONE, {0, 0, 0}};
        Say.Cells(Cells, Row, 0, Line);
    }
    Say.Caret(Cells, painter::core::Point{2, 1});
}

/* One run's frames: what the display said each cost, and how long each took
 * from its step to its frame told back. */
struct Run {
    std::vector<std::uint32_t> Micros, Pixels, Bytes, Records, Round;

    void Add(const Event &Shown, std::uint32_t RoundMicros)
    {
        Micros.push_back(Shown.Micros), Pixels.push_back(Shown.Pixels), Bytes.push_back(Shown.Bytes);
        Records.push_back(Shown.Records), Round.push_back(RoundMicros);
    }
    static std::uint64_t Mean(const std::vector<std::uint32_t> &Of)
    {
        std::uint64_t Sum = 0;
        for (std::uint32_t Each : Of) Sum += Each;
        return Of.empty() ? 0 : Sum / Of.size();
    }
    static std::uint32_t Most(const std::vector<std::uint32_t> &Of) { return Of.empty() ? 0 : *std::max_element(Of.begin(), Of.end()); }
    void Say(const char *What) const
    {
        std::printf("itemcheck: %s, %zu steps — %llu us a frame composed and put on the glass (most %u), %llu pixels, "
                    "%llu bytes and %llu records of the ring a step, %llu us from a step to its frame told back\n",
                    What, Micros.size(), static_cast<unsigned long long>(Mean(Micros)), Most(Micros),
                    static_cast<unsigned long long>(Mean(Pixels)), static_cast<unsigned long long>(Mean(Bytes)),
                    static_cast<unsigned long long>(Mean(Records)), static_cast<unsigned long long>(Mean(Round)));
    }
};

/* A step put in the ring and its frame waited for, counted in Into. False
 * when it was never told shown. */
template <typename Change> bool Stepped(Area &On, Told &Heard, Run &Into, Change &&Say)
{
    Say();
    const auto          Start  = std::chrono::steady_clock::now();
    const std::uint64_t Before = On.Step();
    (void)On.Present();
    /* What is said as it was goes nowhere: a step of nothing is no step. */
    if (On.Step() == Before) {
        Heard.Same = true;
        return false;
    }
    const std::optional<Event> Shown = ShownTo(On, On.Step(), Heard);
    if (!Shown) return false;
    const auto Took = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - Start).count();
    Into.Add(*Shown, static_cast<std::uint32_t>(std::clamp<std::int64_t>(Took, 0, UINT32_MAX)));
    return true;
}

}  // namespace

int main()
{
    std::expected<Area, box::error> Opened = Area::Open(painter::gui::Placement::Over, PointF{Wide, High});
    if (!Opened) {
        std::printf("itemcheck: FAIL — the display opened no area: %.*s\n", static_cast<int>(Opened.error().message().size()),
                    Opened.error().message().data());
        return 1;
    }
    Area      &On    = *Opened;
    const bool Seen  = On.On() == painter::gui::Glass::Pixels;
    const auto Shown = static_cast<std::uint64_t>(On.Size().X * On.Scale()) * static_cast<std::uint64_t>(On.Size().Y * On.Scale());
    std::printf("itemcheck: an area of %.0fx%.0f points at %.2f pixels a point, on %s\n", On.Size().X, On.Size().Y, On.Scale(),
                Seen ? "a framebuffer" : "a text page");
    const std::optional<std::uint32_t> Picture = Seen ? Checkerboard(On) : std::nullopt;
    if (Seen && !Picture) std::printf("itemcheck: the display gave no block for a picture; the scene is shown without it\n");

    Told Heard;
    Run  First, Word, Move, Fade, Cell;
    bool All = Stepped(On, Heard, First, [&] { Scene(On, On.Size().X, On.Size().Y, Picture); });
    if (All) First.Say("the scene");
    for (std::uint32_t I = 0; All && I < Steps; I++)
        All = Stepped(On, Heard, Word, [&] {
            char Time[8];
            std::snprintf(Time, sizeof Time, "12:%02u", (I + 1) % 60);
            On.Text(Clock, Time);
        });
    if (All) Word.Say("one word changed");
    for (std::uint32_t I = 0; All && I < Steps; I++)
        All = Stepped(On, Heard, Move, [&] {
            On.Place(Bubble, {On.Size().X * 0.62f - static_cast<float>((I + 1) * 3), 24.0f + static_cast<float>((I + 1) * 2)});
        });
    if (All) Move.Say("a shape moved");
    for (std::uint32_t I = 0; All && I < Steps; I++)
        All = Stepped(On, Heard, Fade, [&] { On.Opacity(Glass, 0.2f + 0.8f * static_cast<float>(I) / Steps); });
    if (All) Fade.Say("a group faded");
    for (std::uint32_t I = 0; All && I < Steps; I++)
        All = Stepped(On, Heard, Cell, [&] {
            const ItemCell One{static_cast<std::uint32_t>(U'0' + I % 10), 0x00FFD040u, 0x00402010u, 0, ITEM_MARK_NONE, {0, 0, 0}};
            On.Cells(Cells, I % CellsHigh, I % CellsWide, std::span(&One, 1));
        });
    if (All) Cell.Say("one cell changed");

    if (!All) {
        std::printf("itemcheck: FAIL — %s\n", Heard.Same ? "a step said only what was said, and nothing went"
                                               : Heard.Lost ? "a step was never told shown: the area has no bell"
                                                            : "a step was never told shown");
        return 1;
    }
    const std::uint64_t Small = Run::Mean(Word.Pixels), Tiny = Run::Mean(Cell.Pixels);
    if (Heard.Refused || (Seen && (Small == 0 || Small * 8 > Shown || Tiny == 0 || Tiny * 64 > Shown))) {
        std::printf("itemcheck: FAIL — %u changes refused; a word changed composed %llu pixels of the area's %llu, a cell "
                    "changed %llu\n",
                    Heard.Refused, static_cast<unsigned long long>(Small), static_cast<unsigned long long>(Shown),
                    static_cast<unsigned long long>(Tiny));
        return 1;
    }

    if (!CheckForProgrammer()) {
        std::printf("itemcheck: FAIL — painter::gui for the programmer did not hold\n");
        return 1;
    }

    /* The console: lines printed, then an area in its flow after them — it
     * opens once every line before it is drawn — and its first step shown:
     * the glass then shows them all. */
    const auto Printing = std::chrono::steady_clock::now();
    for (std::uint32_t I = 0; I < Lines; I++) std::printf("the console's line %u of %u, as itemcheck prints it\n", I + 1, Lines);
    std::expected<Area, box::error> Flowing = Area::Open(painter::gui::Placement::Flow, PointF{0, 40});
    if (!Flowing) {
        std::printf("itemcheck: FAIL — the display opened no area in the console's flow\n");
        return 1;
    }
    Flowing->Create(1, Kind::Text, ITEM_AREA, ITEM_TOP, Role::Body);
    Flowing->Text(1, "in the console's flow");
    (void)Flowing->Present();
    if (!ShownTo(*Flowing, Flowing->Step(), Heard)) {
        std::printf("itemcheck: FAIL — the area in the console's flow was never told shown\n");
        return 1;
    }
    const auto Took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - Printing).count();
    std::printf("itemcheck: the console, %u lines and an area in its flow — %lld ms until the glass showed them\n", Lines,
                static_cast<long long>(Took));
    std::printf("itemcheck: PASS — every step shown, nothing refused%s\n",
                Seen ? ", a word changed composing a small part of the area" : "; a text page composes no pixels");
    return 0;
}
