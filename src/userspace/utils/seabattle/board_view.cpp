/* board_view.cpp — the waters' picture (board_view.h). */

#include "board_view.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <string>

#include "painter/tui/text.h"

#include "hud.h"
#include "sea.h"
#include "ship_art.h"

namespace seabattle {

namespace {

namespace tui = painter::tui;
namespace core = painter::core;

constexpr int GridLineShare = 30;

float CellX(const GridView &At, int Col) { return static_cast<float>(At.X + Col * At.Cell) + static_cast<float>(At.Cell) * 0.5f; }
float CellY(const GridView &At, int Row) { return static_cast<float>(At.Y + Row * At.Cell) + static_cast<float>(At.Cell) * 0.5f; }

bool Hidden(const BoardScene &Scene, int Row, int Col) { return Row == Scene.HideRow && Col == Scene.HideCol; }

void PaintGridLines(Canvas &Out, const GridView &At)
{
    const int Wide = At.Pixels();
    for (int Line = 0; Line <= At.Size; Line++) {
        const int Along = Line * At.Cell;
        for (int Step = 0; Step < Wide; Step++) {
            Out.Blend(At.X + Along, At.Y + Step, Rgb(0xFF, 0xFF, 0xFF), GridLineShare);
            Out.Blend(At.X + Step, At.Y + Along, Rgb(0xFF, 0xFF, 0xFF), GridLineShare);
        }
    }
}

// Tongues of fire standing on a cell, each flickering to a beat of its own, and a light on the water.
void PaintFlames(Canvas &Out, float X, float Y, int Cell, std::uint64_t Ms, std::uint32_t Seed, Color Fire, bool Text)
{
    if (Cell < 4) {
        Out.Put(static_cast<int>(X), static_cast<int>(Y), (Ms / 120 + Seed) % 2 == 0 ? Fire : Lighter(Fire, 0.5f));
        return;
    }
    const auto Size = static_cast<float>(Cell);
    if (!Text) Out.Glow(X, Y, Size * 0.95f, Fire, 40 + static_cast<int>(40.0f * Beat(Ms, 700, static_cast<float>(Seed & 7) * 0.13f)));
    const float Wide = Cell >= 7 ? 2.0f : 1.0f;
    for (int Tongue = -1; Tongue <= 1; Tongue++) {
        const auto  Shift = static_cast<float>(Tongue);
        const float Phase = static_cast<float>(Seed & 7) * 0.1f + Shift * 0.27f;
        const float Reach = Tongue == 0 ? 0.5f : 0.34f;
        const float Height = Size * (Reach * 0.7f + Reach * 0.5f * Beat(Ms, 280 + (Tongue + 1) * 90, Phase));
        const int   Column = static_cast<int>(X + Shift * Size * 0.2f + Wave(static_cast<float>(Ms % 800) / 800.0f + Phase) * 0.7f - Wide * 0.5f + 0.5f);
        for (int Up = 0; static_cast<float>(Up) < Height; Up++) {
            const float Along = static_cast<float>(Up) / Height;
            const Color Ink = Along < 0.35f ? Mixed(Rgb(0xB8, 0x24, 0x14), Fire, Along / 0.35f) : Mixed(Fire, Rgb(0xFF, 0xE8, 0x70), (Along - 0.35f) / 0.65f);
            for (int Across = 0; static_cast<float>(Across) < Wide; Across++) Out.Put(Column + Across, static_cast<int>(Y + Size * 0.2f) - Up, Ink);
        }
    }
}

// A burning piece of a ship the shooter cannot see whole: charred plating under the flames.
void PaintWreckage(Canvas &Out, float X, float Y, int Cell)
{
    const auto Size = static_cast<float>(Cell);
    for (int Row = -Cell / 2; Row <= Cell / 2; Row++) {
        for (int Col = -Cell / 2; Col <= Cell / 2; Col++) {
            const float Far = std::sqrt(static_cast<float>(Row * Row + Col * Col)) - Size * 0.34f;
            if (Far > 0.0f) continue;
            const float Grit = 0.7f + 0.55f * Grain(static_cast<int>(X) + Col, static_cast<int>(Y) + Row, 9);
            Out.Put(static_cast<int>(X) + Col, static_cast<int>(Y) + Row, Toned(Rgb(0x34, 0x2E, 0x2C), Grit));
        }
    }
}

void PaintShips(Canvas &Out, const BoardScene &Scene)
{
    const std::span<const Ship> Ships = Scene.Waters.Ships();
    for (std::size_t Index = 0; Index < Ships.size(); Index++) {
        const Ship &Which = Ships[Index];
        const bool  Going = static_cast<int>(Index) == Scene.Sinking;
        if (!Scene.ShowShips && !Which.Sunk() && !Going) continue;
        DrawShip(Out, ShipDraw{.X = Scene.At.X + Which.Col * Scene.At.Cell,
                               .Y = Scene.At.Y + Which.Row * Scene.At.Cell,
                               .Cell = Scene.At.Cell,
                               .Length = Which.Length,
                               .Vertical = Which.Vertical,
                               .Steel = Scene.Shown.Ship,
                               .Water = Scene.Shown.Sea,
                               .Ms = Scene.Ms,
                               .Wreck = Which.Sunk() && !Going,
                               .Sink = Going ? Scene.Sink : 0.0f,
                               .Plain = Scene.Text});
    }
}

void PaintMarks(Canvas &Out, const BoardScene &Scene)
{
    const GridView &At = Scene.At;
    for (int Row = 0; Row < At.Size; Row++) {
        for (int Col = 0; Col < At.Size; Col++) {
            if (Hidden(Scene, Row, Col)) continue;
            const Mark Known = Scene.Waters.MarkAt(Row, Col);
            const auto Seed = static_cast<std::uint32_t>(Hash(Row, Col));
            if (Known == Mark::Halo) {
                Out.Cover(static_cast<int>(CellX(At, Col)), static_cast<int>(CellY(At, Row)), Scene.Shown.Splash, 0.45f);
            } else if (Known == Mark::Hit && Scene.Waters.ShipAt(Row, Col) != Scene.Sinking) {
                if (!Scene.ShowShips) PaintWreckage(Out, CellX(At, Col), CellY(At, Row), At.Cell);
                PaintFlames(Out, CellX(At, Col), CellY(At, Row), At.Cell, Scene.Ms, Seed, Scene.Shown.Fire, Scene.Text);
            } else if (Known == Mark::Wreck && ((Row + Col) & 1) == 0) {
                const bool Going = Scene.Waters.ShipAt(Row, Col) == Scene.Sinking;
                if (!Going) Out.Cover(static_cast<int>(CellX(At, Col)), static_cast<int>(CellY(At, Row)), Mixed(Scene.Shown.Fire, Rgb(0xC8, 0x30, 0x18), 0.5f), 0.55f * Beat(Scene.Ms, 900, static_cast<float>(Seed & 7) * 0.12f));
            }
        }
    }
}

void PutSmoke(tui::FineCanvas &Out, const BoardScene &Scene, int Row, int Col, int Puffs)
{
    const GridView &At = Scene.At;
    const auto      Size = static_cast<float>(At.Cell);
    for (int Puff = 0; Puff < Puffs; Puff++) {
        const float Phase = static_cast<float>(Scene.Ms % 3200) / 3200.0f + Grain(Row, Col, 30 + Puff);
        const float Q = Phase - static_cast<float>(static_cast<int>(Phase));
        const float X = CellX(At, Col) + Wave(Q * 1.3f + Grain(Row, Col, 40 + Puff)) * Size * 0.22f;
        const float Y = CellY(At, Row) - Size * 0.15f - Q * Size * 1.5f;
        Out.Disc(X, Y, Size * (0.1f + 0.22f * Q), Rgb(0x3A, 0x3A, 0x40), 0.42f * (1.0f - Q));
    }
}

}  // namespace

void PaintBoard(Canvas &Out, const BoardScene &Scene)
{
    const GridView &At = Scene.At;
    PaintSea(Out, SeaPatch{At.X, At.Y, At.Pixels(), At.Pixels(), Scene.Shown.Sea, 0.5f, 0.9f, Scene.Text}, Scene.Ms);
    PaintGridLines(Out, At);
    PaintShips(Out, Scene);
    PaintMarks(Out, Scene);
}

void ShapeBoard(tui::FineCanvas &Out, const BoardScene &Scene)
{
    const GridView &At = Scene.At;
    const auto      Size = static_cast<float>(At.Cell);
    for (int Row = 0; Row < At.Size; Row++) {
        for (int Col = 0; Col < At.Size; Col++) {
            if (Hidden(Scene, Row, Col)) continue;
            const Mark Known = Scene.Waters.MarkAt(Row, Col);
            if (Known == Mark::Miss) {
                const float Breath = 1.0f + 0.07f * Wave(static_cast<float>(Scene.Ms % 2400) / 2400.0f + Grain(Row, Col, 5));
                const float Outer = Size * 0.32f * Breath;
                Out.Ring(CellX(At, Col), CellY(At, Row), Outer, std::max(0.0f, Outer - 0.9f), Scene.Shown.Splash, 0.8f);
                Out.Disc(CellX(At, Col), CellY(At, Row), Size * 0.09f + 0.3f, Scene.Shown.Splash, 0.8f);
            } else if (Known == Mark::Hit && Scene.Waters.ShipAt(Row, Col) != Scene.Sinking) {
                PutSmoke(Out, Scene, Row, Col, 2);
            } else if (Known == Mark::Wreck && ((Row + Col) & 1) == 0 && Scene.Waters.ShipAt(Row, Col) != Scene.Sinking) {
                PutSmoke(Out, Scene, Row, Col, 1);
            }
        }
    }
}

void LabelBoard(tui::Frame &Out, const Look &Shown, const GridView &At, std::string_view Title, int Row, int Col)
{
    const core::Rect Box = At.Frame();
    PaintFrame(Out, Box, Mixed(Shown.Title, Shown.Bars, 0.45f), Shown.Bars);
    const std::int32_t TitleRow = Box.Y - 2, LetterRow = Box.Y - 1;
    Out.Fill(core::Rect{Box.X - 2, TitleRow, Box.Width + 2, 1}, ' ', {Shown.Text, Shown.Bars});
    tui::PutText(Out, core::Point{Box.X, TitleRow}, Title, Box.Width, Shown.Title, Shown.Bars);
    Out.Fill(core::Rect{Box.X - 2, LetterRow, Box.Width + 2, 1}, ' ', {Shown.Text, Shown.Bars});
    for (int Index = 0; Index < At.Size; Index++) {
        const char Letter = static_cast<char>('A' + Index);
        const std::int32_t Column = At.X + Index * At.Cell + At.Cell / 2;
        Out.Put({Column, LetterRow}, Letter, {Index == Col ? Shown.Title : Shown.Quiet, Shown.Bars});
        const std::int32_t Line = (At.Y + Index * At.Cell + At.Cell / 2) / 2;
        const std::string  Number = std::to_string(Index + 1);
        tui::PutText(Out, core::Point{Box.X - 1 - static_cast<std::int32_t>(Number.size()), Line}, Number, 2, Index == Row ? Shown.Title : Shown.Quiet, Shown.Bars);
    }
}

}  // namespace seabattle
