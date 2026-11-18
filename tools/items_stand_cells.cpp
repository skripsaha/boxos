/* items stand — cells and the console on the glass: a cells item's grid, runs
 * and caret said by a program (painter/gui/changes.h) arrive as it said them,
 * only what changed going into the ring, the last word kept when the ring is
 * full, every bound and lie refused; and the frame (frame.h) — the console the
 * lowest item, areas over it and in its flow, one caret, the mirror moved
 * where the console scrolled (mirror.h) — holds, after each step of random
 * typing, scrolling and changing, the very pixels of the glass composed whole
 * from nothing, and puts on the glass every pixel that changed and few that
 * did not. */

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "frame.h"
#include "items_stand.h"
#include "mirror.h"
#include "typesetter.h"

namespace stand {

namespace {

using painter::core::Color;
using painter::core::Rect;
using painter::gui::Changes;
using painter::gui::Kind;
using painter::gui::Role;

ItemCell CellOf(std::uint32_t Character, std::uint32_t Ink, std::uint32_t Paper, std::uint8_t Mark = ITEM_MARK_NONE)
{
    return ItemCell{Character, Ink, Paper, Mark ? 0x00F0C040u : 0u, Mark, {0, 0, 0}};
}

/* A cell made at random: a character, colours of every kind a cell takes. */
ItemCell RandomCell()
{
    static const std::uint32_t Colours[] = {COLOR_DEFAULT, COLOR_THROUGH, 0x00FF8040u, 0x002060C0u,
                                            BoxColorTranslucent(0x0040C080u, 32), 0x00FFFFFFu};
    const std::uint32_t Pick = Random();
    if (Pick % 7 == 0) return item_cell_empty();
    const std::uint32_t Characters[] = {U'a', U'Z', U'▀', U'█', U' ', U'Ж', U'#'};
    return CellOf(Characters[(Pick >> 3) % 7], Colours[(Pick >> 6) % 6], Colours[(Pick >> 9) % 6],
                  static_cast<std::uint8_t>((Pick >> 12) % 5 == 0 ? ITEM_MARK_UNDER : ITEM_MARK_NONE));
}

bool Same(const ItemCell &A, const ItemCell &B) { return std::memcmp(&A, &B, sizeof(ItemCell)) == 0; }

/* Whether the tree's grid of Item holds Want, Cols x Rows. */
bool Holds(const items::AreaState &Area, std::uint32_t Item, const std::vector<ItemCell> &Want, std::uint32_t Cols)
{
    const items::Item *Of = Area.Items.Find(Item);
    if (!Of || !Of->Grid || Of->Grid->Count() != Want.size() || Of->Grid->Cols() != Cols) return false;
    for (std::size_t I = 0; I < Want.size(); I++)
        if (!Same(Of->Grid->At(I), Want[I])) return false;
    return true;
}

void SayRows(Changes &Say, std::uint32_t Item, const std::vector<ItemCell> &Cells, std::uint32_t Cols)
{
    for (std::uint32_t Row = 0; Row * Cols < Cells.size(); Row++)
        Say.Cells(Item, Row, 0, std::span(Cells.data() + static_cast<std::size_t>(Row) * Cols, Cols));
}

/* A record written straight into the bench's ring, as a program that breaks
 * the language would. */
void Raw(Bench &On, std::uint16_t Op, std::uint32_t Item, const std::vector<std::uint8_t> &Body)
{
    std::vector<std::uint8_t> Made((sizeof(ItemRecord) + Body.size() + 7u) & ~std::size_t{7}, 0);
    const ItemRecord          Head{Op, static_cast<std::uint16_t>(Made.size() / 8u), Item};
    std::memcpy(Made.data(), &Head, sizeof(Head));
    std::memcpy(Made.data() + sizeof(Head), Body.data(), Body.size());
    std::uint8_t *Ring  = On.Memory.data() + sizeof(ItemAreaHeader);
    std::uint64_t Write = On.Head->Write;
    std::uint8_t *At    = item_ring_place(Ring, On.RingBytes, &Write, On.Head->Read, static_cast<std::uint32_t>(Made.size()));
    if (At) std::memcpy(At, Made.data(), Made.size());
    On.Head->Write = Write;
}

void CheckLanguage(Tally &Count)
{
    const items::Theme  Look;
    items::ProgramState Program = HostProgram(Look, 1.0f);
    Bench               On(Program);
    Changes             Say;
    const std::uint32_t Cols = 10, Rows = 4;
    std::vector<ItemCell> Want(Cols * Rows);
    for (ItemCell &Each : Want) Each = RandomCell();
    Say.Create(1, Kind::Cells, ITEM_AREA, ITEM_TOP, Role::Code);
    Say.Grid(1, Cols, Rows);
    SayRows(Say, 1, Want, Cols);
    Say.Caret(1, painter::core::Point{3, 2});
    On.Step(Say);
    Count.Check(Holds(On.Area, 1, Want, Cols) && On.Refusals.empty(), "cells: a grid and its cells said arrive as said");
    const items::Item *Item = On.Area.Items.Find(1);
    Count.Check(Item && Item->Grid && Item->Grid->CaretShown() && Item->Grid->Caret().Col == 3 && Item->Grid->Caret().Row == 2,
                "cells: the caret said arrives where it was said");

    /* What did not change is not said again; one cell changed is one cell. */
    std::uint64_t Before = Say.Written();
    SayRows(Say, 1, Want, Cols);
    On.Step(Say);
    Count.Check(Say.Written() == Before, "cells: a frame said again unchanged writes nothing at all");
    Want[17] = CellOf(U'Q', 0x00FFFF00u, COLOR_DEFAULT);
    Before   = Say.Written();
    SayRows(Say, 1, Want, Cols);
    On.Step(Say);
    const std::uint64_t OneCell = (sizeof(ItemRecord) + sizeof(ItemCellsRun) + sizeof(ItemCell) + 7u) / 8u * 8u + 16u;
    Count.Check(Holds(On.Area, 1, Want, Cols) && Say.Written() - Before == OneCell,
                "cells: one cell changed is one run of one cell, " + std::to_string(Say.Written() - Before) + " bytes with its step");

    /* A ring too small for a frame: each step what fits, the last word kept,
     * the grid at last the last frame said. */
    Bench   Small(Program, 4096);
    Changes Tight;
    const std::uint32_t Wide = 120, High = 40;
    std::vector<ItemCell> Frame(Wide * High);
    Tight.Create(1, Kind::Cells, ITEM_AREA, ITEM_TOP, Role::Code);
    Tight.Grid(1, Wide, High);
    for (int Round = 0; Round < 3; Round++) {
        for (ItemCell &Each : Frame) Each = RandomCell();
        SayRows(Tight, 1, Frame, Wide);
        Small.Step(Tight);
    }
    int Steps = 0;
    while (!Tight.Empty() && Steps++ < 400) Small.Step(Tight);
    Count.Check(Tight.Empty() && Holds(Small.Area, 1, Frame, Wide) && Small.Refusals.empty(),
                "cells: through a ring too small for a frame, the last frame said arrives whole (" + std::to_string(Steps) +
                    " steps)");

    /* Refused: a grid on what is not cells; a grid past what a program may
     * hold; a run past its row; cells written before a grid. */
    Bench   Bad(Program);
    Changes Wrong;
    Wrong.Create(2, Kind::Shape);
    Wrong.Create(3, Kind::Cells);
    Wrong.Grid(2, 4, 4);
    Bad.Step(Wrong);
    const bool NotCells = Bad.Refusals.size() == 1 && Bad.Refusals[0] == ERR_INVALID_ARGUMENT;
    Bad.Refusals.clear();
    ItemGrid Huge{4096, 4096};
    std::vector<std::uint8_t> Body(sizeof(ItemChange) + sizeof(ItemGrid));
    const ItemChange Change{ITEM_PROP_GRID, 0, sizeof(ItemGrid)};
    std::memcpy(Body.data(), &Change, sizeof(Change));
    std::memcpy(Body.data() + sizeof(Change), &Huge, sizeof(Huge));
    Raw(Bad, ITEM_OP_CHANGE, 3, Body);
    ItemCellsRun Before3{0, 0, 1, 0};
    std::vector<std::uint8_t> Early(sizeof(ItemCellsRun) + sizeof(ItemCell));
    std::memcpy(Early.data(), &Before3, sizeof(Before3));
    Raw(Bad, ITEM_OP_CELLS, 3, Early);
    std::vector<std::uint8_t> Whole(sizeof(ItemStep));
    const ItemStep            Number{1000};
    std::memcpy(Whole.data(), &Number, sizeof(Number));
    Raw(Bad, ITEM_OP_STEP, 0, Whole);
    Changes Step;
    Bad.Step(Step);
    Changes Small2;
    Small2.Grid(3, 4, 2);
    Bad.Step(Small2);
    ItemCellsRun Past{1, 3, 2, 0};
    std::vector<std::uint8_t> Over(sizeof(ItemCellsRun) + 2 * sizeof(ItemCell));
    std::memcpy(Over.data(), &Past, sizeof(Past));
    Raw(Bad, ITEM_OP_CELLS, 3, Over);
    Changes Step2;
    Step2.Caret(3, painter::core::Point{0, 0});
    Bad.Step(Step2);
    Count.Check(NotCells && Bad.Refusals.size() == 3 && Bad.Refusals[0] == ERR_QUOTA_EXCEEDED &&
                    Bad.Refusals[1] == ERR_INVALID_STATE && Bad.Refusals[2] == ERR_INVALID_ARGUMENT,
                "cells: a grid on a shape, a grid past the bound, cells before a grid and a run past its row are refused");

    /* At random: runs of every length and place, the grid the runs said. */
    Bench   Fuzz(Program);
    Changes Random2;
    const std::uint32_t FC = 37, FR = 11;
    std::vector<ItemCell> Kept(FC * FR, item_cell_empty());
    Random2.Create(4, Kind::Cells);
    Random2.Grid(4, FC, FR);
    for (int Round = 0; Round < 300; Round++) {
        const std::uint32_t Row = Random() % FR, Col = Random() % FC, Length = 1 + Random() % (FC - Col);
        std::vector<ItemCell> Run(Length);
        for (ItemCell &Each : Run) Each = RandomCell();
        std::copy(Run.begin(), Run.end(), Kept.begin() + Row * FC + Col);
        Random2.Cells(4, Row, Col, Run);
        if (Round % 7 == 0) Fuzz.Step(Random2);
    }
    Fuzz.Step(Random2);
    Count.Check(Holds(Fuzz.Area, 4, Kept, FC) && Fuzz.Refusals.empty(), "cells: 300 runs at random arrive as their grid");
}

/* ---- The frame ------------------------------------------------------------ */

/* Text into the console as a lane says it. */
void Say(ConsoleModel &Console, const std::string &Text, std::uint32_t Ink = COLOR_DEFAULT, std::uint32_t Paper = COLOR_DEFAULT)
{
    for (std::size_t At = 0; At < Text.size(); At += CONSOLE_RUN_TEXT_MAX) {
        ConsoleRun Run{};
        Run.kind = CONSOLE_RUN_TEXT;
        Run.len  = static_cast<std::uint8_t>(std::min<std::size_t>(CONSOLE_RUN_TEXT_MAX, Text.size() - At));
        Run.fg   = Ink;
        Run.bg   = Paper;
        std::memcpy(Run.text, Text.data() + At, Run.len);
        Console.Apply(Run, 1);
    }
}

std::string RandomText()
{
    static const char *Words[] = {"items ", "cells ", "glass ", "\xD0\xBA\xD0\xBE\xD0\xBD\xD1\x81\xD0\xBE\xD0\xBB\xD1\x8C ", "mirror ",
                                  "# ", "\n", "\n", "scroll ", "BoxOS ", "\xE2\x96\x80\xE2\x96\x88 "};
    std::string Made;
    const std::uint32_t Count = 1 + Random() % 30;
    for (std::uint32_t I = 0; I < Count; I++) Made += Words[Random() % 11];
    return Made;
}

/* The glass as the easel would have it: every stretch the mirror said
 * waits, copied. */
void Flushed(Mirror &From, std::vector<std::uint32_t> &Glass, std::uint64_t &Written)
{
    From.TakeWaiting([&](std::uint32_t Y, std::int32_t X0, std::int32_t X1) {
        std::copy(From.Row(Y) + X0, From.Row(Y) + X1, Glass.begin() + static_cast<std::ptrdiff_t>(Y * From.Width() + X0));
        Written += static_cast<std::uint64_t>(X1 - X0);
    });
}

void CheckFrame(Tally &Count)
{
    Typesetter Type(std::nullopt, 16);
    FrameGlass Glass;
    Glass.Width  = 643;
    Glass.Height = 405;
    Glass.Type   = &Type;
    std::shared_ptr<CellGrid> Grid = FrameStart(Glass);
    ConsoleModel              Console(*Grid);
    const CellsLook          &Look = FrameCells();
    Count.Check(Grid->Cols() == 643u / static_cast<std::uint32_t>(Look.Width) && Look.Grid.X == (643 - Look.Grid.Width) / 2 &&
                    Look.Grid.Y == (405 - Look.Grid.Height) / 2,
                "frame: the console as many cells as the glass holds, in its middle");

    /* Over the console: an area of a panel, a text and cells of their own;
     * in its flow, an area of cells moving with it. */
    items::ProgramState Program = HostProgram(FrameTheme(), 1.25f);
    Program.From.Cells          = &Look;
    Bench   Over(Program), Flow(Program);
    Changes Up, Along;
    Over.Area.Place = Rect{180, 90, 300, 170};
    Over.Area.Shown = true;
    Up.Create(1, Kind::Shape, ITEM_AREA, ITEM_TOP, Role::Panel);
    Up.Size(1, {200, 90});
    Up.Corners(1, painter::core::Corners::All(10));
    Up.Shadows(1, std::vector<painter::gui::Shadow>{painter::gui::Shadow{{0, 4}, 8, 0, Color::Rgba(0, 0, 0, 120), false}});
    Up.Opacity(1, 0.85f);
    Up.Create(2, Kind::Text, ITEM_AREA, ITEM_TOP, Role::Body);
    Up.Place(2, {12, 10});
    Up.Text(2, "over the console");
    /* A plate that hides what lies beneath it whole: it stays still when the
     * console scrolls under it. */
    Up.Create(4, Kind::Shape);
    Up.Place(4, {150, 20});
    Up.Size(4, {60, 40});
    const painter::gui::Fill Solid = painter::gui::Fill::Of(Color::Rgb(40, 90, 160));
    Up.Fills(4, std::span(&Solid, 1));
    Up.Create(3, Kind::Cells, ITEM_AREA, ITEM_TOP, Role::Code);
    Up.Grid(3, 14, 3);
    std::vector<ItemCell> Mine(14 * 3);
    for (ItemCell &Each : Mine) Each = RandomCell();
    SayRows(Up, 3, Mine, 14);
    Up.Caret(3, painter::core::Point{4, 1});
    Over.Step(Up);
    Say(Console, "BoxOS console\nline two\n");
    const std::uint64_t FlowLine = Console.Reserve(3);
    Flow.Area.Place = Rect{Look.Grid.X, Look.Grid.Y + static_cast<std::int32_t>(FlowLine - Console.RowsGone()) * Look.Height, Look.Grid.Width,
                           3 * Look.Height - 5};
    Flow.Area.Shown = true;
    Along.Create(1, Kind::Cells, ITEM_AREA, ITEM_TOP, Role::Code);
    Along.Grid(1, 20, 2);
    std::vector<ItemCell> Flowing(40);
    for (ItemCell &Each : Flowing) Each = RandomCell();
    SayRows(Along, 1, Flowing, 20);
    Flow.Step(Along);

    std::vector<items::AreaState *> Programs{&Flow.Area, &Over.Area};
    Mirror                          Shown(Glass.Width, Glass.Height);
    std::vector<std::uint32_t>      Seen(static_cast<std::size_t>(Glass.Width) * Glass.Height, 0);
    std::uint64_t                   Written = 0, Shifted = 0, Scrolls = 0, Composed = 0;
    auto Frame = [&] {
        (void)FrameDue(Console, true);
        Composed += FrameCompose(Shown, Programs).Pixels;
        Flushed(Shown, Seen, Written);
    };
    Frame();
    bool Whole = true, Glassy = true, Plain = true;
    int  Failures = 0;
    double ScrollMicros = 0, WholeMicros = 0;
    for (int Round = 0; Round < 120; Round++) {
        const std::uint64_t Gone = Console.RowsGone();
        switch (Random() % 8) {
        case 6: {
            /* More than a screen at once: nothing of what the glass shows is
             * left to move. */
            std::string Burst;
            for (int Line = 0; Line < 40; Line++) Burst += "burst line " + std::to_string(Line) + (Line % 3 ? "\n" : " of more words\n");
            Say(Console, Burst);
            break;
        }
        case 7:
            if (Random() % 4 == 0) {
                ConsoleRun Clear{};
                Clear.kind = CONSOLE_RUN_CLEAR;
                Clear.fg = Clear.bg = COLOR_DEFAULT;
                Console.Apply(Clear, 1);
            } else {
                Say(Console, RandomText());
            }
            break;
        case 0:
        case 1:
        case 2: {
            /* Text in colours of every kind a lane may send: the screen's
             * own, a triple, and what the console's wire does not carry —
             * see-through, and half see-through — which it holds as the
             * screen's own. */
            static const std::uint32_t Colours[] = {COLOR_DEFAULT, 0x0060E0A0u, 0x00302040u, COLOR_THROUGH,
                                                    BoxColorTranslucent(0x00C04020u, 24)};
            Say(Console, RandomText(), Colours[Random() % 5], Colours[Random() % 5]);
            break;
        }
        case 3:
            Mine[Random() % Mine.size()] = RandomCell();
            SayRows(Up, 3, Mine, 14);
            Up.Caret(3, painter::core::Point{static_cast<std::int32_t>(Random() % 14), static_cast<std::int32_t>(Random() % 3)});
            Over.Step(Up);
            break;
        case 4:
            Up.Text(2, std::string("step ") + std::to_string(Round));
            Up.Place(1, {static_cast<float>(Random() % 60), static_cast<float>(Random() % 40)});
            if (Random() % 2) Up.Place(4, {100.0f + static_cast<float>(Random() % 120), static_cast<float>(Random() % 90)});
            Over.Step(Up);
            break;
        default: {
            ConsoleRun Step{};
            Step.kind = CONSOLE_RUN_STEP;
            Step.len  = 4;
            const std::int32_t Delta = -static_cast<std::int32_t>(Random() % 5);
            std::memcpy(Step.text, &Delta, sizeof(Delta));
            Console.Apply(Step, 1);
            break;
        }
        }
        /* The area in the flow goes up with the console's rows, as the
         * display lays it (areas.h AreasFlow). */
        const std::uint64_t Moved = Console.RowsGone() - Gone;
        Flow.Area.Place.Y -= static_cast<std::int32_t>(Moved) * Look.Height;
        const std::uint64_t Before = Written;
        const auto          Start  = std::chrono::steady_clock::now();
        Frame();
        const double Micros = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - Start).count();
        if (Moved != 0 && Moved < Grid->Rows()) {
            Scrolls++;
            Shifted += Written - Before;
            ScrollMicros += Micros;
        }
        /* The same glass composed whole from nothing. */
        Mirror     Fresh(Glass.Width, Glass.Height);
        const auto Again = std::chrono::steady_clock::now();
        FrameEverything();
        (void)FrameCompose(Fresh, Programs);
        WholeMicros += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - Again).count();
        const bool Alike = std::equal(Shown.Pixels(), Shown.Pixels() + Seen.size(), Fresh.Pixels());
        const bool Put   = std::equal(Seen.begin(), Seen.end(), Shown.Pixels());
        if ((!Alike || !Put) && Failures++ < 3)
            std::printf("  step %d: %s%s\n", Round, Alike ? "" : "the mirror is not the glass composed whole; ",
                        Put ? "" : "a changed pixel never went onto the glass");
        Whole  = Whole && Alike;
        Glassy = Glassy && Put;
        for (std::size_t I = 0; I < Grid->Count(); I++) {
            const ItemCell &Cell = Grid->At(I);
            Plain = Plain && ((Cell.Ink >> 24) == 0 || Cell.Ink == COLOR_DEFAULT) && ((Cell.Paper >> 24) == 0 || Cell.Paper == COLOR_DEFAULT);
        }
    }
    Count.Check(Plain, "frame: the console holds every colour plain — a triple or the screen's own — see-through and half "
                       "see-through ones said in its text among them");
    Count.Check(Whole, "frame: after each of 120 steps of typing in colours of every kind, scrolling, a caret moved and areas "
                       "changed — a plate kept still over the console among them — the mirror holds the very pixels of the glass "
                       "composed whole");
    Count.Check(Glassy, "frame: every pixel that changed went onto the glass");
    const std::uint64_t Pixels = static_cast<std::uint64_t>(Glass.Width) * Glass.Height;
    Count.Check(Scrolls > 5, "frame: the console scrolled " + std::to_string(Scrolls) + " times in the steps");
    if (Scrolls)
        std::printf("  a scroll wrote %.1f%% of the glass on average; on this host %.0f us a scroll frame, %.0f us the glass "
                    "composed whole\n",
                    100.0 * static_cast<double>(Shifted) / static_cast<double>(Scrolls * Pixels), ScrollMicros / static_cast<double>(Scrolls),
                    WholeMicros / 120.0);
    Count.Check(Scrolls == 0 || Shifted < Scrolls * Pixels, "frame: a scroll writes less than the whole glass");

    /* The console written straight into the mirror where nothing lies over
     * it holds the very pixels the canvas's way gives: a see-through veil
     * over the whole glass sends every tile that way. */
    Mirror Straight(Glass.Width, Glass.Height), Canvassed(Glass.Width, Glass.Height);
    FrameEverything();
    (void)FrameCompose(Straight, Programs);
    Bench   Veil(Program);
    Changes See;
    Veil.Area.Place = Rect{0, 0, static_cast<std::int32_t>(Glass.Width), static_cast<std::int32_t>(Glass.Height)};
    Veil.Area.Shown = true;
    See.Create(1, Kind::Shape);
    See.Size(1, {static_cast<float>(Glass.Width) / 1.25f, static_cast<float>(Glass.Height) / 1.25f});
    const painter::gui::Fill Clear = painter::gui::Fill::Of(Color::Rgba(0, 0, 0, 0));
    See.Fills(1, std::span(&Clear, 1));
    Veil.Step(See);
    std::vector<items::AreaState *> Veiled{&Flow.Area, &Over.Area, &Veil.Area};
    FrameEverything();
    (void)FrameCompose(Canvassed, Veiled);
    Count.Check(std::equal(Straight.Pixels(), Straight.Pixels() + Seen.size(), Canvassed.Pixels()),
                "frame: the console written straight into the mirror, the very pixels the canvas's way gives");
    FrameEverything();
    (void)FrameCompose(Shown, Programs);

    /* One caret on the glass: the cells over the console that show one hold
     * it, the console's goes; hidden, the console's comes back. */
    const ItemCaret Console1 = Grid->Caret();
    Count.Check(!Grid->Holds() && Over.Area.Items.Find(3)->Grid->Holds() && Console1.Shown,
                "frame: one caret on the glass — the cells over the console that show one");
    Up.Hidden(3, true);
    Over.Step(Up);
    Frame();
    Count.Check(Grid->Holds() && !Over.Area.Items.Find(3)->Grid->Holds(), "frame: the cells hidden, the console's caret is the one");
}

}  // namespace

void CheckCells(Tally &Count)
{
    CheckLanguage(Count);
    CheckFrame(Count);
}

}  // namespace stand
