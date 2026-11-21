/* cells.cpp — the lamp in the console's cells. See cells.h.
 *
 * A cell is 1 across and 2 down in the units the cells draw in (PutPixel's),
 * so the lamp, 1 tall, is 2 x Rows of them tall; its field is worked out at
 * the middle of every part of every cell, and each part coloured as the
 * pixels' face colours a pixel. */

#include "cells.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "box/clock.h"
#include "painter/tui/fine_canvas.h"
#include "painter/tui/mosaic.h"
#include "painter/tui/text.h"
#include "painter/tui/window.h"

#include "look.h"

namespace lavalamp {

namespace {

namespace tui = painter::tui;

constexpr std::uint64_t FrameUs  = 40000;     // 25 frames a second
constexpr std::uint64_t KeysUs   = 5000000;   // the line of keys, so long after the start and after the panel closes
constexpr float         Faint    = 0.002f;
constexpr float         Sharp    = 0.02f;
constexpr int           BarCells = 10;

painter::core::Color ColorOf(Rgb C)
{
    const auto Byte = [](float Of) { return static_cast<std::uint8_t>(std::lround(std::clamp(Of, 0.0f, 1.0f) * 255.0f)); };
    return painter::core::Color::Rgb(Byte(C.R), Byte(C.G), Byte(C.B));
}

int PartsOf(const tui::Stage &On)
{
    if (On.Glass() == tui::GlassKind::Text) return 2;
    return (On.Mosaics() & ITEM_MOSAIC_OCTANTS) != 0 ? 4 : (On.Mosaics() & ITEM_MOSAIC_SEXTANTS) != 0 ? 3 : 2;
}

}  // namespace

CellLamp::CellLamp(tui::Stage Opened, const Composition &Start)
    : Screen(std::move(Opened)), Current(Start), Wax(static_cast<float>(Screen.Size().Width) / (2.0f * static_cast<float>(Screen.Size().Height)), Start),
      Colours(Start)
{
    Cols  = Screen.Size().Width;
    Rows  = Screen.Size().Height;
    Parts = PartsOf(Screen);
    const float Across = 1.0f / (4.0f * static_cast<float>(Rows)), Down = 1.0f / (static_cast<float>(Parts) * static_cast<float>(Rows));
    Grid.Lay(Cols * 2, Rows * Parts, Across / 2, Down / 2, Across, Down);
    Lit.assign(static_cast<std::size_t>(Cols) * Rows, 0);
    WasLit = Lit;
}

void CellLamp::Retune()
{
    Wax.Tune(Current);
    Colours = Shades(Current);
    SlopeBy = SlopeOf * Wax.BlobSize();
    Behind.resize(static_cast<std::size_t>(Grid.Across()) * Grid.Down());
    for (int Y = 0; Y < Grid.Down(); Y++)
        for (int X = 0; X < Grid.Across(); X++) {
            const float U = Grid.X(X) / Wax.Aspect();
            const Rgb   Back = Colours.Behind(U, Grid.Y(Y));
            const float Sheen = Colours.Sheen(U);
            Behind[static_cast<std::size_t>(Y) * Grid.Across() + X] = Rgb{Back.R + Sheen, Back.G + Sheen, Back.B + Sheen};
        }
    Whole = true;
}

void CellLamp::Paint()
{
    tui::Frame     &Out = Screen.FrameBuffer();
    tui::FineCanvas Fine(Out);
    const int       Wide = Grid.Across(), High = Grid.Down();
    const float     PerX = SlopeBy / (2.0f * (Grid.X(1) - Grid.X(0))), PerY = SlopeBy / (2.0f * (Grid.Y(1) - Grid.Y(0)));
    for (int Row = 0; Row < Rows; Row++)
        for (int Col = 0; Col < Cols; Col++) {
            const std::size_t Cell = static_cast<std::size_t>(Row) * Cols + Col;
            float             Most = 0;
            for (int Part = 0; Part < 2 * Parts; Part++) Most = std::max(Most, Grid.Level(Col * 2 + Part % 2, Row * Parts + Part / 2));
            Lit[Cell] = Most > Faint;
            const bool Shaded = Tuned.Open() && Col >= Window.X && Col < Window.X + Window.Width && Row >= Window.Y && Row < Window.Y + Window.Height;
            if (!Whole && !Lit[Cell] && !WasLit[Cell] && !Shaded) continue;
            tui::Mosaic Wanted;
            Wanted.Rows = static_cast<std::uint8_t>(Parts);
            for (std::size_t Part = 0; Part < Wanted.Count(); Part++) {
                const int   X = Col * 2 + static_cast<int>(Part % 2), Y = Row * Parts + static_cast<int>(Part / 2);
                const float Level = Grid.Level(X, Y);
                const Rgb   Back  = Behind[static_cast<std::size_t>(Y) * Wide + X];
                if (Level <= Faint) {
                    Wanted.At[Part] = ColorOf(Back);
                    continue;
                }
                const int   L = std::max(X - 1, 0), R = std::min(X + 1, Wide - 1), T = std::max(Y - 1, 0), B = std::min(Y + 1, High - 1);
                const float AcrossBy = Grid.Level(R, Y) - Grid.Level(L, Y), DownBy = Grid.Level(X, B) - Grid.Level(X, T);
                const float Edge = std::max(Sharp, std::max(std::fabs(AcrossBy), std::fabs(DownBy)) / 2);
                const Rgb   Waxen = Colours.Wax(Grid.Heat(X, Y), Level, LightOf(Level, AcrossBy * PerX, DownBy * PerY), Grid.Y(Y));
                Wanted.At[Part]   = ColorOf(Colours.Over(Back, Waxen, Level, Edge));
            }
            (void)Fine.PutMosaic({Col, Row}, Wanted);
        }
    std::swap(Lit, WasLit);
    Whole = false;
}

/* The panel: a window in the middle, a row for each thing the lamp is made
 * of — its name, a swatch or a bar, its value — the chosen one marked. */
void CellLamp::PaintPanel()
{
    tui::Frame &Out = Screen.FrameBuffer();
    if (!Tuned.Open()) {
        (void)Out.SetCaret(std::nullopt);
        return;
    }
    const std::int32_t Wide = std::min(Cols, std::max<std::int32_t>(46, static_cast<std::int32_t>(tui::CellsOf(Tuned.Hint())) + 6));
    const painter::core::Rect Area   = tui::Centred(Out.Size(), Wide, lavalamp::Rows + 4);
    Window                           = {Area.X, Area.Y, Area.Width + 1, Area.Height + 1};
    const painter::core::Rect Inside = tui::DrawWindow(Out, Area, "lavalamp", "", Tuned.Hint());
    const std::int32_t        Swatch = Inside.X + 12, Right = Inside.X + Inside.Width - 1;
    for (int At = 0; At < lavalamp::Rows; At++) {
        const Row          Of     = static_cast<Row>(At);
        const std::int32_t Y      = Inside.Y + At;
        const bool         Marked = Of == Tuned.Chosen();
        const painter::core::Color Paper = Marked ? tui::ChosenPaper : tui::WindowPaper;
        const painter::core::Color Ink   = Marked ? tui::TitleInk : tui::WindowInk;
        (void)Out.Fill({Inside.X, Y, Inside.Width, 1}, U' ', {Ink, Paper});
        (void)Out.Put({Inside.X, Y}, Marked ? U'>' : U' ', {Ink, Paper});
        (void)tui::PutText(Out, {Inside.X + 2, Y}, Tuning::Name(Of), 9, Ink, Paper);
        if (Of == Row::Palette) {
            const painter::core::Color Three[] = {Current.Wax, Current.Rim, Current.Glass};
            for (int Cell = 0; Cell < 3; Cell++) (void)Out.Put({Swatch + Cell, Y}, U' ', {Ink, Three[Cell]});
        } else if (Tuning::IsColor(Of)) {
            (void)Out.Fill({Swatch, Y, 3, 1}, U' ', {Ink, Tuning::ColorOf(Of, Current)});
        } else {
            const int Filled = std::max(1, static_cast<int>(std::lround(Tuning::Share(Of, Current) * BarCells)));
            for (int Cell = 0; Cell < BarCells; Cell++)
                (void)Out.Put({Swatch + Cell, Y}, Cell < Filled ? U'═' : U'─', {Cell < Filled ? painter::core::Colors::Amber : tui::HintInk, Paper});
        }
        const bool        Typing = Tuned.Typing() && Marked;
        const std::string Value  = Typing ? std::string(Tuned.Typed()) : Tuning::Value(Of, Current);
        const std::int32_t From  = Right - static_cast<std::int32_t>(tui::CellsOf(Value));
        (void)tui::PutText(Out, {From, Y}, Value, static_cast<std::int32_t>(tui::CellsOf(Value)), Typing ? painter::core::Colors::Amber : Ink, Paper);
        if (Typing) (void)Out.SetCaret(painter::core::Point{Right, Y});
    }
    if (!Tuned.Typing()) (void)Out.SetCaret(std::nullopt);
    const std::int32_t NoteY = Inside.Y + lavalamp::Rows;
    (void)Out.Fill({Inside.X, NoteY, Inside.Width, 1}, U' ', {tui::WindowInk, tui::WindowPaper});
    (void)tui::PutText(Out, {Inside.X + 2, NoteY}, Tuned.Note(), Inside.Width - 2, painter::core::Colors::Coral, tui::WindowPaper);
}

/* The line of keys at the bottom, a while after the start and after the panel
 * closes. */
void CellLamp::PaintKeys()
{
    const bool Shown = !Tuned.Open() && clock_uptime_us() < KeysUntilUs;
    /* Gone, the line leaves the lamp's cells under it to be drawn again. */
    if (KeysShown && !Shown) Whole = true;
    KeysShown = Shown;
    if (!Shown) return;
    tui::Frame &Out = Screen.FrameBuffer();
    (void)Out.Centre(Rows - 1, std::string(" ") + std::string(Tuned.Hint()) + " ", {tui::WindowInk, tui::WindowPaper});
}

Composition CellLamp::Run(bool &Closed)
{
    Screen.HearEnd();
    Retune();
    std::uint64_t Last = clock_uptime_us(), Next = Last;
    KeysUntilUs        = Last + KeysUs;
    for (bool Going = true; Going;) {
        if (Watch.changed()) {
            Current = ReadLook().Lamp;
            Retune();
        }
        const std::uint64_t Now = clock_uptime_us();
        if (Now >= Next) {
            Wax.Advance(static_cast<float>(Now - Last) / 1e6f);
            Last = Now;
            Grid.Fill(Wax);
            Paint();
            PaintPanel();
            PaintKeys();
            if (!Screen.Present()) break;
            /* The next frame from the clock after this one is drawn: on a
             * wide screen a frame may take longer than a frame's time, and
             * the keys must still be heard between them. */
            Next = clock_uptime_us() + FrameUs;
            continue;
        }
        const tui::Event Heard = Screen.Wait(static_cast<std::uint32_t>(std::max<std::uint64_t>(1, (Next - Now) / 1000)));
        if (Heard.Kind == tui::EventKind::Gone || Heard.Kind == tui::EventKind::EndAsked) break;
        if (Heard.Kind != tui::EventKind::Key) continue;
        const bool Open = Tuned.Open();
        switch (Tuned.Key(Heard.Key, Current)) {
        case lavalamp::Heard::Leave: Going = false; break;
        case lavalamp::Heard::Lamp: Retune(); [[fallthrough]];
        case lavalamp::Heard::Panel:
            /* Closed, the panel leaves the cells under it to be drawn again. */
            if (Open && !Tuned.Open()) {
                KeysUntilUs = clock_uptime_us() + KeysUs;
                Whole       = true;
            }
            Next = Now;
            break;
        case lavalamp::Heard::Nothing: break;
        }
    }
    Closed = static_cast<bool>(Screen.Close());
    return Current;
}

}  // namespace lavalamp
