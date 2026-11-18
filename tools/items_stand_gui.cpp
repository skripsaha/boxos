/*
 * items_stand_gui.cpp — painter::gui as a program writes with it, held on the
 * host: items as objects, made and told and gone; frames described whole and
 * only what differs sent, the area then the very picture one frame drawn from
 * nothing makes; a text measured beforehand the very size the display lays it
 * at.
 */

#include <algorithm>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "items_stand.h"
#include "painter/core/system_fonts.h"
#include "painter/gui/frame.h"
#include "painter/gui/items.h"
#include "painter/gui/measure.h"

/* The system's fonts, as the stand has them: the tree's own — the faces the
 * display's side here lays texts in. */
namespace painter::core {
std::shared_ptr<const typecase::Fonts> SystemFonts() { return stand::TreeFonts(); }
}  // namespace painter::core

namespace stand {

namespace {

using painter::core::Color;
using painter::core::PointF;
using painter::core::Rect;
using painter::gui::ByContent;
using painter::gui::Changes;
using painter::gui::Role;
namespace gui = painter::gui;

constexpr std::uint32_t First = 1u << 20;   // the first number painter gives an item

/* An area's items composed whole onto a canvas W x H. */
painter::core::Canvas Composed(items::AreaState &Area, const items::Theme &Look, std::int32_t W, std::int32_t H)
{
    const Rect             Whole{0, 0, W, H};
    items::Compositor      Composer(Whole);
    items::AreaState *const Areas[] = {&Area};
    Composer.Take(Areas, Look);
    painter::core::Canvas Made = *painter::core::Canvas::Make(static_cast<std::uint32_t>(W), static_cast<std::uint32_t>(H));
    Made.Clear(Color::Rgb(20, 22, 30));
    Composer.Compose(Made, painter::core::Point{}, Whole, Areas, Look);
    return Made;
}

bool SamePixels(const painter::core::Canvas &A, const painter::core::Canvas &B)
{
    for (std::uint32_t Y = 0; Y < A.Height(); Y++)
        for (std::uint32_t X = 0; X < A.Width(); X++)
            if (!(A.Row(Y)[X] == B.Row(Y)[X])) return false;
    return true;
}

/* ---- Objects ----------------------------------------------------------------- */

void CheckObjects(Tally &Count)
{
    const items::Theme  Look;
    items::ProgramState Program = HostProgram(Look, 1.0f);
    Bench               Shown(Program);
    Changes             Said;
    gui::detail::Numbers Pool;
    std::optional<gui::detail::Holder> Holder;
    Holder.emplace(Said, Pool);

    gui::Text Time = Holder->Text("12:46", Role::Heading);
    Time.SetPlace({16, 8});
    gui::Group Box   = Holder->Group();
    gui::Shape Plate = Box.Rectangle(Role::Panel);
    Plate.SetSize({120, 40}).SetCorners(painter::core::Corners::All(6));
    (void)Shown.Step(Said);
    const items::Item *T = Shown.Area.Items.Find(First), *G = Shown.Area.Items.Find(First + 1), *P = Shown.Area.Items.Find(First + 2);
    Count.Check(T && T->Kind == ITEM_TEXT && T->Role == ITEM_ROLE_HEADING && T->Said.Text && *T->Said.Text == "12:46" && T->Said.Place &&
                    T->Said.Place->X == 16 * ITEM_POINT && G && G->Kind == ITEM_GROUP && P && P->Kind == ITEM_SHAPE &&
                    P->Parent == First + 1 && Shown.Refusals.empty(),
                "objects: a text, a group and a shape in it made and said as objects, numbered by painter");

    Time.SetText("12:46").SetPlace({16, 8});
    Plate.SetSize({120, 40});
    Count.Check(Said.Empty(), "objects: items told again what they say write nothing");
    Time.SetText("12:47");
    Count.Check(Said.Waiting() == 1, "objects: of an item told all of itself, only what changed waits");
    (void)Shown.Step(Said);

    std::size_t Before = 0;
    {
        gui::Text Passing = Holder->Text("passing by");
        (void)Shown.Step(Said);
        Before = Shown.Area.Items.Count();
    }
    (void)Shown.Step(Said);
    Count.Check(Shown.Area.Items.Count() + 1 == Before, "objects: an object gone takes its item with it");

    gui::Text Moved = std::move(Time);
    Moved.SetText("12:48");
    Time.SetText("moved from");
    (void)Shown.Step(Said);
    T = Shown.Area.Items.Find(First);
    Count.Check(T && T->Said.Text && *T->Said.Text == "12:48" && !Time,
                "objects: an object moved keeps its item, what it was moved from holds none");

    /* A group gone first takes what lies in it; their objects then say
     * nothing of it as they go. */
    gui::Shape Inside = Box.Ellipse();
    (void)Shown.Step(Said);
    const std::size_t Refused = Shown.Refusals.size();
    Box.Remove();
    (void)Shown.Step(Said);
    Plate  = gui::Shape();
    Inside = gui::Shape();
    (void)Shown.Step(Said);
    Count.Check(Shown.Refusals.size() == Refused && !Shown.Area.Items.Find(First + 1) && !Shown.Area.Items.Find(First + 2),
                "objects: a group gone takes what lies in it, and their objects going say nothing the display refuses");

    Holder.reset();
    Moved.SetText("after its area");
    Count.Check(!Moved && Said.Waiting() == 0, "objects: an object outliving its area does nothing");
}

/* ---- Frames ------------------------------------------------------------------ */

/* An item of a scene: a text, a rectangle or a group, under the item it names
 * (or the area), keyed or not. */
struct Node {
    int           Kind   = 0;    // 0 a text, 1 a rectangle, 2 a group
    int           Parent = -1;   // an earlier node, a group; -1 the area
    bool          Keyed  = false;
    std::uint64_t Key    = 0;
    std::string   Words;
    PointF        At{}, Size{40, 20};
    Color         Fill = Color::Rgb(90, 140, 210);
    bool          Hidden = false;
    Role          As     = Role::None;
};

std::string RandomWords()
{
    static const char *Words[] = {"items", "frame", "кадр", "glass", "12:46", "painter", "Duppo", "строка"};
    std::string Made;
    for (std::uint32_t I = 0, Many = 1 + Random() % 3; I < Many; I++) Made += std::string(I ? " " : "") + Words[Random() % 8];
    return Made;
}

Node RandomNode(const std::vector<Node> &Scene, std::uint64_t &Keys)
{
    Node Made;
    Made.Kind = static_cast<int>(Random() % 3);
    std::vector<int> Groups;
    for (int I = 0; I < static_cast<int>(Scene.size()); I++)
        if (Scene[static_cast<std::size_t>(I)].Kind == 2) Groups.push_back(I);
    Made.Parent = !Groups.empty() && Random() % 2 ? Groups[Random() % Groups.size()] : -1;
    Made.Keyed  = Random() % 3 == 0;
    Made.Key    = Keys++;
    Made.Words  = RandomWords();
    Made.At     = PointF{static_cast<float>(Random() % 300), static_cast<float>(Random() % 160)};
    Made.Size   = PointF{static_cast<float>(10 + Random() % 120), static_cast<float>(8 + Random() % 60)};
    Made.Fill   = Color::Rgb(static_cast<std::uint8_t>(Random()), static_cast<std::uint8_t>(Random()), static_cast<std::uint8_t>(Random()));
    Made.As     = Made.Kind == 0 ? (Random() % 2 ? Role::Body : Role::Heading) : (Random() % 3 ? Role::None : Role::Panel);
    return Made;
}

/* The scene described in a frame: each node made under its parent's object,
 * parents before what lies in them. */
void Describe(gui::Frame &Into, const std::vector<Node> &Scene)
{
    std::vector<gui::Group> Groups(Scene.size());
    for (std::size_t I = 0; I < Scene.size(); I++) {
        const Node &Each = Scene[I];
        const auto  Made = [&](auto &Maker) {
            if (Each.Kind == 0) {
                gui::Text One = Maker.Text(Each.Words, Each.As);
                One.SetPlace(Each.At);
                if (Each.Keyed) One.Key(Each.Key);
                if (Each.Hidden) One.Hide();
            } else if (Each.Kind == 1) {
                gui::Shape One = Maker.Rectangle(Each.As);
                One.SetPlace(Each.At).SetSize(Each.Size).SetFill(gui::Fill::Of(Each.Fill));
                if (Each.Keyed) One.Key(Each.Key);
                if (Each.Hidden) One.Hide();
            } else {
                if constexpr (std::is_same_v<std::remove_cvref_t<decltype(Maker)>, gui::Group>) Groups[I] = Maker.Subgroup(Each.As);
                else Groups[I] = Maker.Group(Each.As);
                Groups[I].SetPlace(Each.At).SetOpacity(Each.Hidden ? 0.5f : 1.0f);
                if (Each.Keyed) Groups[I].Key(Each.Key);
            }
        };
        if (Each.Parent < 0) Made(Into);
        else Made(Groups[static_cast<std::size_t>(Each.Parent)]);
    }
}

/* A scene changed a little: words, places, fills, an item hidden, made,
 * gone, keyed items reordered. */
void Change(std::vector<Node> &Scene, std::uint64_t &Keys)
{
    for (std::uint32_t K = 0, Many = 1 + Random() % 4; K < Many && !Scene.empty(); K++) {
        Node &Each = Scene[Random() % Scene.size()];
        switch (Random() % 6) {
        case 0: Each.Words = RandomWords(); break;
        case 1: Each.At = PointF{static_cast<float>(Random() % 300), static_cast<float>(Random() % 160)}; break;
        case 2: Each.Fill = Color::Rgb(static_cast<std::uint8_t>(Random()), 80, static_cast<std::uint8_t>(Random())); break;
        case 3: Each.Hidden = !Each.Hidden; break;
        case 4: Each.As = Each.Kind == 0 ? (Each.As == Role::Body ? Role::Heading : Role::Body) : Each.As; break;
        default: Each.Size = PointF{static_cast<float>(10 + Random() % 120), static_cast<float>(8 + Random() % 60)}; break;
        }
    }
    if (Random() % 3 == 0) Scene.push_back(RandomNode(Scene, Keys));
    if (Random() % 4 == 0 && Scene.size() > 2) {
        /* A node gone, and what lies in it — the parents' numbers after it
         * counted again. */
        const std::size_t Gone = Random() % Scene.size();
        std::vector<bool> Removed(Scene.size(), false);
        std::vector<int>  NewIndex(Scene.size(), -1);
        std::vector<Node> Kept;
        for (std::size_t I = 0; I < Scene.size(); I++) {
            const Node &Each = Scene[I];
            Removed[I]       = I == Gone || (Each.Parent >= 0 && Removed[static_cast<std::size_t>(Each.Parent)]);
            if (Removed[I]) continue;
            Node Moved   = Each;
            Moved.Parent = Each.Parent < 0 ? -1 : NewIndex[static_cast<std::size_t>(Each.Parent)];
            NewIndex[I]  = static_cast<int>(Kept.size());
            Kept.push_back(Moved);
        }
        Scene = std::move(Kept);
    }
    if (Random() % 3 == 0) {
        /* The keyed items under the area swapped about, as a list sorted
         * otherwise. */
        std::vector<std::size_t> Keyed;
        for (std::size_t I = 0; I < Scene.size(); I++)
            if (Scene[I].Keyed && Scene[I].Parent < 0 && Scene[I].Kind != 2) Keyed.push_back(I);
        if (Keyed.size() >= 2) std::swap(Scene[Keyed[0]], Scene[Keyed[Keyed.size() - 1]]);
    }
}

void CheckFrames(Tally &Count)
{
    const items::Theme  Look;
    items::ProgramState Program = HostProgram(Look, 1.25f);
    Bench               Shown(Program);
    Shown.Area.Place = Rect{0, 0, 420, 240};
    Shown.Area.Shown = true;
    Changes              Said;
    gui::detail::Numbers Pool;
    gui::detail::Holder  Holder(Said, Pool);
    gui::detail::Drawer  Frames(Holder.Where());

    std::uint64_t     Keys = 1;
    std::vector<Node> Scene;
    for (int I = 0; I < 12; I++) Scene.push_back(RandomNode(Scene, Keys));
    bool Alike = true, Still = true, Items = true;
    int  Failures = 0;
    for (int Round = 0; Round < 120; Round++) {
        Describe(Frames.Begin(), Scene);
        Frames.End();
        (void)Shown.Step(Said);
        /* The same frame again: nothing to say. */
        Describe(Frames.Begin(), Scene);
        Frames.End();
        Still = Still && Said.Empty();

        /* The very frame drawn from nothing. */
        items::ProgramState Program2 = HostProgram(Look, 1.25f);
        Bench               Fresh(Program2);
        Fresh.Area.Place = Shown.Area.Place;
        Fresh.Area.Shown = true;
        Changes              Said2;
        gui::detail::Numbers Pool2;
        gui::detail::Holder  Holder2(Said2, Pool2);
        gui::detail::Drawer  Frames2(Holder2.Where());
        Describe(Frames2.Begin(), Scene);
        Frames2.End();
        (void)Fresh.Step(Said2);
        const bool Same = SamePixels(Composed(Shown.Area, Look, 420, 240), Composed(Fresh.Area, Look, 420, 240));
        const bool Many = Shown.Area.Items.Count() == Fresh.Area.Items.Count();
        if ((!Same || !Many) && Failures++ < 3)
            std::printf("  frame %d: %s%s (%zu items, %zu drawn fresh)\n", Round, Same ? "" : "the picture differs ",
                        Many ? "" : "the items differ", Shown.Area.Items.Count(), Fresh.Area.Items.Count());
        Alike = Alike && Same;
        Items = Items && Many;
        Change(Scene, Keys);
    }
    Count.Check(Alike && Items && Shown.Refusals.empty(),
                "frames: 120 frames of a scene changing — words, places, fills, hidden, made, gone, a list reordered — "
                "each sending only what differs, the area every time the very items and pixels the frame drawn from "
                "nothing makes, nothing refused");
    Count.Check(Still, "frames: a frame described as the one before says nothing");
}

/* ---- Measuring ---------------------------------------------------------------- */

void CheckMeasure(Tally &Count)
{
    const items::Theme  Look;
    items::ProgramState Program = HostProgram(Look, 1.25f);
    Bench               Shown(Program);
    Shown.Area.Place = Rect{0, 0, 900, 600};
    Shown.Area.Shown = true;
    static const Role Roles[] = {Role::Body, Role::Heading, Role::Code, Role::Label, Role::Hint, Role::Button};
    int               Same = 0, Tried = 0;
    for (std::uint32_t Item = 1; Item <= 150; Item++) {
        std::string Words = RandomWords();
        if (Random() % 4 == 0) Words += "\n" + RandomWords() + " " + RandomWords();
        const Role  As    = Roles[Random() % 6];
        const float Width = Random() % 3 ? ByContent : static_cast<float>(40 + Random() % 260);
        gui::TextStyle Style;
        if (Random() % 3 == 0) Style.Size = static_cast<float>(9 + Random() % 30);
        if (Random() % 4 == 0) Style.Weight = static_cast<std::uint16_t>(Random() % 2 ? 700 : 500);
        if (Random() % 5 == 0) Style.Italic = true;
        if (Random() % 6 == 0) Style.LetterSpacing = static_cast<float>(Random() % 3);

        const PointF Measured = gui::detail::MeasureOn(gui::Glass::Pixels, Words, As, Width, Style);
        Changes      Say;
        Say.Create(Item, gui::Kind::Text, ITEM_AREA, ITEM_TOP, As);
        Say.Text(Item, Words);
        Say.Size(Item, PointF{Width, ByContent});
        Say.Style(Item, Style);
        (void)Shown.Step(Say);
        (void)Composed(Shown.Area, Look, 900, 600);
        const items::Item *Laid = Shown.Area.Items.Find(Item);
        Tried++;
        if (Laid && Laid->Drawn && Measured.X == static_cast<float>(Laid->Drawn->Measured.X) / ITEM_POINT &&
            Measured.Y == static_cast<float>(Laid->Drawn->Measured.Y) / ITEM_POINT)
            Same++;
        else if (Tried - Same <= 3)
            std::printf("  measured %.3f x %.3f, laid %.3f x %.3f: \"%s\"\n", Measured.X, Measured.Y,
                        Laid && Laid->Drawn ? Laid->Drawn->Measured.X / 256.0 : -1.0, Laid && Laid->Drawn ? Laid->Drawn->Measured.Y / 256.0 : -1.0,
                        Words.c_str());
    }
    Count.Check(Same == Tried, "measure: " + std::to_string(Tried) +
                                   " texts of every role, wrapped or not, in styles of their own — each measured beforehand the "
                                   "very size the display lays it at (" + std::to_string(Same) + " alike)");
    const PointF Page = gui::detail::MeasureOn(gui::Glass::Text, "ab\ncde", Role::Body, ByContent, {});
    Count.Check(Page.X == 3 * ITEM_PAGE_CELL_WIDE && Page.Y == 2 * ITEM_PAGE_CELL_TALL,
                "measure: on a text page a character a cell, a line a row");
}

/* ---- A canvas's part ---------------------------------------------------------- */

void CheckCanvasPart(Tally &Count)
{
    const items::Theme    Look;
    items::ProgramState   Program = HostProgram(Look, 1.0f);
    painter::core::Canvas Page    = *painter::core::Canvas::Make(160, 100);
    Page.Clear(Color::Rgb(10, 20, 30));
    Program.From.CanvasOf = [&](std::uint32_t Block) -> const painter::core::Canvas * { return Block == 7 ? &Page : nullptr; };
    Bench Shown(Program);
    Shown.Area.Place = Rect{0, 0, 320, 200};
    Shown.Area.Shown = true;
    Changes Say;
    Say.Create(1, gui::Kind::Canvas);
    Say.Place(1, {40, 30});
    Say.Canvas(1, 7);
    (void)Shown.Step(Say);

    const Rect              Whole{0, 0, 320, 200};
    const Color             Ground = Color::Rgb(20, 22, 30);
    items::Compositor       Composer(Whole);
    items::AreaState *const Areas[] = {&Shown.Area};
    painter::core::Canvas   Screen = *painter::core::Canvas::Make(320, 200);
    Screen.Clear(Ground);
    std::uint64_t Composed = 0;
    const auto    Frame    = [&] {
        Composer.Take(Areas, Look);
        for (const Rect &Each : Composer.Rects()) {
            Screen.Clear(Each, Ground);
            Composer.Compose(Screen, painter::core::Point{}, Each, Areas, Look);
            Composed += static_cast<std::uint64_t>(Each.Width) * static_cast<std::uint64_t>(Each.Height);
        }
    };
    Frame();
    bool          Alike = true;
    std::uint64_t Said  = 0;
    Composed            = 0;
    for (int Round = 0; Round < 40; Round++) {
        const Rect Part{static_cast<std::int32_t>(Random() % 140), static_cast<std::int32_t>(Random() % 80),
                        static_cast<std::int32_t>(1 + Random() % 20), static_cast<std::int32_t>(1 + Random() % 20)};
        Page.Clear(Part, Color::Rgb(static_cast<std::uint8_t>(Random()), static_cast<std::uint8_t>(Random()), 200));
        Shown.Area.Items.BlockChanged(7, Part);
        Said += static_cast<std::uint64_t>(Part.Width) * static_cast<std::uint64_t>(Part.Height);
        Frame();
        painter::core::Canvas Fresh = *painter::core::Canvas::Make(320, 200);
        Fresh.Clear(Ground);
        Composer.Compose(Fresh, painter::core::Point{}, Whole, Areas, Look);
        Alike = Alike && SamePixels(Screen, Fresh);
    }
    Count.Check(Alike && Composed == Said,
                "canvas: 40 parts of a canvas changed, each composed alone — " + std::to_string(Composed) + " pixels for the " +
                    std::to_string(Said) + " that changed — the screen the very pixels composed whole");
}

}  // namespace

void CheckGui(Tally &Count)
{
    CheckObjects(Count);
    CheckFrames(Count);
    CheckMeasure(Count);
    CheckCanvasPart(Count);
}

}  // namespace stand
