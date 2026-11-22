/* pixels.cpp — the lamp in the screen's own pixels. See pixels.h.
 *
 * The field is worked out at the corners of cells Step pixels square, and a
 * pixel inside a cell takes its level and its wax's colour between the four
 * corners' — so the edge of the wax is still placed to the pixel, softened
 * over one, while the field and the light are worked out once a cell. The
 * liquid and the glass behind are the same from frame to frame, a row's and
 * a column's part of them kept in tables. */

#include "pixels.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "box/clock.h"
#include "look.h"

namespace lavalamp {

namespace {

constexpr float Faint  = 0.002f;   // a level below which nothing of the wax shows, not even its glow
constexpr float Sharp  = 0.004f;   // the least a level changes across a pixel, for a flat cell's edge
constexpr int   Pixels = 140000;   // about this many points in the grid, whatever the screen

/* A key, or the keyboard gone, as Wait tells it. */
gui::Event AsEvent(const painter::tui::Event &Typed)
{
    gui::Event Made;
    Made.What = Typed.Kind == painter::tui::EventKind::Key        ? gui::Event::Kind::Key
              : Typed.Kind == painter::tui::EventKind::EndAsked ? gui::Event::Kind::EndAsked
                                                                : gui::Event::Kind::Gone;
    Made.Key  = Typed.Key;
    return Made;
}

painter::core::Pixel PixelOf(Rgb C)
{
    const auto Part = [](float Of) {
        Of = Of < 0.0f ? 0.0f : Of > 1.0f ? 1.0f : Of;
        return static_cast<std::uint16_t>(Of * 65535.0f + 0.5f);
    };
    return painter::core::Pixel{Part(C.R), Part(C.G), Part(C.B), 65535};
}

}  // namespace

PixelLamp::PixelLamp(gui::Area Opened, painter::tui::Keyboard Heard, const Composition &Start)
    : Screen(std::move(Opened)), Keys(std::move(Heard)), Current(Start), Wax(Screen.Size().X / Screen.Size().Y, Start), Colours(Start)
{
    Under = Screen.Group();
    Pane.emplace(Screen);
}

void PixelLamp::Build()
{
    const float Scale = Screen.Scale();
    Wide     = std::max(1, static_cast<int>(std::lround(Screen.Size().X * Scale)));
    High     = std::max(1, static_cast<int>(std::lround(Screen.Size().Y * Scale)));
    Step     = std::clamp(static_cast<int>(std::lround(std::sqrt(static_cast<double>(Wide) * High / Pixels))), 2, 8);
    Cols     = (Wide + Step - 1) / Step;
    CellRows = (High + Step - 1) / Step;
    const float Unit = static_cast<float>(Step) / static_cast<float>(High);
    Grid.Lay(Cols + 1, CellRows + 1, 0, 0, Unit, Unit);
    Wax.Reshape(static_cast<float>(Wide) / static_cast<float>(High));
    Points.assign(static_cast<std::size_t>(Cols + 1) * (CellRows + 1), Rgb{});
    Edges.assign(Points.size(), Sharp);
    Lit.assign(static_cast<std::size_t>(Cols) * CellRows, 0);
    WasLit  = Lit;
    LongLit = Lit;
    Board = Under.Canvas({Wide, High});
    Board.SetPlace({0, 0});
    Retune();
}

void PixelLamp::Retune()
{
    Wax.Tune(Current);
    Colours = Shades(Current);
    SlopeBy = SlopeOf * Wax.BlobSize() * static_cast<float>(High);
    LiquidRow.resize(static_cast<std::size_t>(High));
    BulbRow.resize(static_cast<std::size_t>(High));
    for (int Y = 0; Y < High; Y++) {
        const float Down = (static_cast<float>(Y) + 0.5f) / static_cast<float>(High);
        LiquidRow[static_cast<std::size_t>(Y)] = Colours.Liquid(Down);
        BulbRow[static_cast<std::size_t>(Y)]   = Colours.BulbUp(Down);
    }
    RoundCol.resize(static_cast<std::size_t>(Wide));
    BulbCol.resize(static_cast<std::size_t>(Wide));
    for (int X = 0; X < Wide; X++) {
        const float Across = (static_cast<float>(X) + 0.5f) / static_cast<float>(Wide);
        RoundCol[static_cast<std::size_t>(X)] = Colours.Round(Across);
        BulbCol[static_cast<std::size_t>(X)]  = Colours.BulbAcross(Across);
    }
    Whole = 2;
}

/* Lit: a cell any of whose corners the wax or its glow reaches. Drawn: the
 * cells lit now or in either of the two frames before — every cell whose
 * picture is not the one the page holds. Their bounds, in cells. */
std::optional<painter::core::Rect> PixelLamp::Mark()
{
    int Left = Cols, Top = CellRows, Right = -1, Bottom = -1;
    for (int Row = 0; Row < CellRows; Row++)
        for (int Col = 0; Col < Cols; Col++) {
            const float Most = std::max(std::max(Grid.Level(Col, Row), Grid.Level(Col + 1, Row)),
                                        std::max(Grid.Level(Col, Row + 1), Grid.Level(Col + 1, Row + 1)));
            const std::size_t At = static_cast<std::size_t>(Row) * Cols + Col;
            Lit[At]              = Most > Faint;
            if (Whole == 0 && !Lit[At] && !WasLit[At] && !LongLit[At]) continue;
            Left = std::min(Left, Col), Right = std::max(Right, Col);
            Top = std::min(Top, Row), Bottom = std::max(Bottom, Row);
        }
    if (Right < 0) return std::nullopt;
    return painter::core::Rect{Left, Top, Right - Left + 1, Bottom - Top + 1};
}

/* The wax's colour at each corner the wax reaches within Cells, lit by how
 * steeply its level falls there. */
void PixelLamp::ColourPoints(painter::core::Rect Cells)
{
    const int Across = Cols + 1;
    for (int Y = Cells.Y; Y <= Cells.Y + Cells.Height; Y++)
        for (int X = Cells.X; X <= Cells.X + Cells.Width; X++) {
            const std::size_t At    = static_cast<std::size_t>(Y) * Across + X;
            const float       Level = Grid.Level(X, Y);
            if (Level <= Faint) {
                Points[At] = Rgb{};
                Edges[At]  = Sharp;
                continue;
            }
            const int   L = std::max(X - 1, 0), R = std::min(X + 1, Cols), T = std::max(Y - 1, 0), B = std::min(Y + 1, CellRows);
            const float AcrossBy = (Grid.Level(R, Y) - Grid.Level(L, Y)) / static_cast<float>((R - L) * Step);
            const float DownBy   = (Grid.Level(X, B) - Grid.Level(X, T)) / static_cast<float>((B - T) * Step);
            Points[At] = Colours.Wax(Grid.Heat(X, Y), Level, LightOf(Level, AcrossBy * SlopeBy, DownBy * SlopeBy), Grid.Y(Y));
            Edges[At]  = std::max(Sharp, std::sqrt(AcrossBy * AcrossBy + DownBy * DownBy));
        }
}

void PixelLamp::PaintCell(painter::core::Canvas &Page, int Col, int Row)
{
    const int    Across = Cols + 1;
    const auto   At     = [&](int X, int Y) { return static_cast<std::size_t>(Y) * Across + X; };
    const float  L00 = Grid.Level(Col, Row), L10 = Grid.Level(Col + 1, Row), L01 = Grid.Level(Col, Row + 1), L11 = Grid.Level(Col + 1, Row + 1);
    const Rgb    P00 = Points[At(Col, Row)], P10 = Points[At(Col + 1, Row)], P01 = Points[At(Col, Row + 1)], P11 = Points[At(Col + 1, Row + 1)];
    const float  E00 = Edges[At(Col, Row)], E10 = Edges[At(Col + 1, Row)], E01 = Edges[At(Col, Row + 1)], E11 = Edges[At(Col + 1, Row + 1)];
    const Rgb    Bulb = Colours.BulbLight();
    const int    X0 = Col * Step, Y0 = Row * Step, X1 = std::min(X0 + Step, Wide), Y1 = std::min(Y0 + Step, High);
    const float  Inv = 1.0f / static_cast<float>(Step);
    const bool   Bare = std::max(std::max(L00, L10), std::max(L01, L11)) <= Faint;
    for (int Y = Y0; Y < Y1; Y++) {
        const float T  = static_cast<float>(Y - Y0) * Inv;
        const float LL = L00 + (L01 - L00) * T, LR = L10 + (L11 - L10) * T;
        const float EL = E00 + (E01 - E00) * T, ER = E10 + (E11 - E10) * T;
        const Rgb   CL = Mix(P00, P01, T), CR = Mix(P10, P11, T);
        const Rgb   Liquid = LiquidRow[static_cast<std::size_t>(Y)];
        const float BulbY  = BulbRow[static_cast<std::size_t>(Y)];
        painter::core::Pixel *Line = Page.Row(static_cast<std::uint32_t>(Y));
        for (int X = X0; X < X1; X++) {
            const std::size_t C    = static_cast<std::size_t>(X);
            Rgb               Out  = Plus(Times(Liquid, RoundCol[C]), Times(Bulb, BulbY * BulbCol[C]));
            if (!Bare) {
                const float S = static_cast<float>(X - X0) * Inv;
                Out           = Colours.Over(Out, Mix(CL, CR, S), LL + (LR - LL) * S, EL + (ER - EL) * S);
            }
            Line[X] = PixelOf(Out);
        }
    }
}

void PixelLamp::Paint(painter::core::Canvas &Page, painter::core::Rect Cells)
{
    ColourPoints(Cells);
    for (int Row = Cells.Y; Row < Cells.Y + Cells.Height; Row++)
        for (int Col = Cells.X; Col < Cells.X + Cells.Width; Col++) {
            const std::size_t At = static_cast<std::size_t>(Row) * Cols + Col;
            if (Whole > 0 || Lit[At] || WasLit[At] || LongLit[At]) PaintCell(Page, Col, Row);
        }
}

void PixelLamp::Frame()
{
    const std::uint64_t Now = clock_uptime_us();
    const float Pace = static_cast<float>(Screen.MotionSpeed()) / 100.0f * (Screen.LessMotion() ? 0.5f : 1.0f);
    Wax.Advance(static_cast<float>(Now - LastUs) / 1e6f * Pace);
    LastUs = Now;
    Grid.Fill(Wax);
    if (const std::optional<painter::core::Rect> Cells = Mark()) {
        const painter::core::Rect Covered =
            painter::core::Rect{Cells->X * Step, Cells->Y * Step, Cells->Width * Step, Cells->Height * Step}.Intersect(painter::core::Size{Wide, High});
        const std::optional<painter::core::Rect> Changed = Whole > 0 ? std::nullopt : std::optional(Covered);
        (void)Board.Draw([&](painter::core::Canvas &Page) { Paint(Page, *Cells); }, Changed);
        Whole = std::max(Whole - 1, 0);
    }
    std::swap(LongLit, WasLit);
    std::swap(WasLit, Lit);
}

/* False to leave. */
bool PixelLamp::OnEvent(const gui::Event &Told)
{
    switch (Told.What) {
    case gui::Event::Kind::Gone:
    case gui::Event::Kind::EndAsked: return false;
    case gui::Event::Kind::Key:
        switch (Tuned.Key(Told.Key, Current)) {
        case lavalamp::Heard::Leave: return false;
        case lavalamp::Heard::Lamp: Retune(); [[fallthrough]];
        case lavalamp::Heard::Panel: Pane->Show(Tuned, Current); break;
        case lavalamp::Heard::Nothing: break;
        }
        return true;
    case gui::Event::Kind::Area: {
        const float Scale = Screen.Scale();
        if (std::lround(Told.Size.X * Scale) != Wide || std::lround(Told.Size.Y * Scale) != High) {
            Build();
            Pane->Place();
        }
        if (Hidden && Told.Seen) Seen = Screen.Step();
        Hidden = !Told.Seen;
        LastUs = clock_uptime_us();
        return true;
    }
    case gui::Event::Kind::Shown: Seen = std::max(Seen, Told.Step); return true;
    default: return true;
    }
}

Composition PixelLamp::Run()
{
    Keys.HearEnd();
    Build();
    Pane->Show(Tuned, Current);
    LastUs = clock_uptime_us();
    for (;;) {
        if (Watch.changed()) {
            Current = ReadLook().Lamp;
            Retune();
            Pane->Show(Tuned, Current);
        }
        /* The frame said, the next one waits only for the glass to show the
         * one before it: it is drawn while the display shows this one. */
        const std::uint64_t Before = Screen.Step();
        if (!Hidden) Frame();
        (void)Screen.Present();
        /* All the display told and all that was typed meanwhile taken each
         * frame — the display tells something every frame, and keys waited
         * for only behind it would never be heard — then the glass waited for. */
        for (;;) {
            while (const std::optional<gui::Event> Told = Screen.Next())
                if (!OnEvent(*Told)) return Current;
            for (painter::tui::Event Typed = Keys.Poll(); Typed.Kind != painter::tui::EventKind::Quiet; Typed = Keys.Poll())
                if (!OnEvent(AsEvent(Typed))) return Current;
            if (!Hidden && Seen >= Before) break;
            if (!OnEvent(Screen.Wait(&Keys))) return Current;
        }
    }
}

}  // namespace lavalamp
