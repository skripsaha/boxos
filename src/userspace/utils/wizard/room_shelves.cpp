/* room_shelves.cpp — the things that stand in the room and do not move: the
 * bookshelf with its spines, the window's stone frame, the shelf of jars and
 * the candles' stands. */

#include <algorithm>
#include <array>
#include <cmath>

#include "icons.h"
#include "lore.h"
#include "room_parts.h"

namespace wizard {

namespace {

constexpr std::array<Color, 8> Spines = {Rgb(0x8A, 0x2A, 0x30), Rgb(0x2E, 0x5E, 0x40), Rgb(0x28, 0x3C, 0x74), Rgb(0xB0, 0x82, 0x2E),
                                         Rgb(0x5E, 0x2E, 0x6E), Rgb(0x2C, 0x6A, 0x6E), Rgb(0x6A, 0x44, 0x28), Rgb(0xA8, 0x58, 0x30)};

int StandHeight(const Scene &Where, int Which)
{
    return Which == 0 ? std::max(6, static_cast<int>(Where.Unit * 24.0f)) : 0;
}

int WaxHeight(const Scene &Where, int Which)
{
    return std::max(3, static_cast<int>(Where.Unit * (Which == 0 ? 11.0f : Which == 1 ? 9.0f : 7.0f)));
}

// A row of books on the shelf whose plank is at Plank, Height tall to the one above.
void PaintBookRow(Canvas &Out, int Left, int Right, int Plank, int Height, std::uint32_t Salt)
{
    int X = Left;
    for (int Index = 0; X < Right; Index++) {
        const std::uint32_t Roll = Hash(Index, static_cast<int>(Salt), 41);
        if (Roll % 9u == 0) {                       // a gap, and a book fallen against the next
            X += 1 + static_cast<int>(Roll >> 8) % 2;
            continue;
        }
        const int Wide = std::min(Right - X, 2 + static_cast<int>(Roll >> 4) % 3);
        const int High = std::max(3, Height - 2 - static_cast<int>(Roll >> 12) % std::max(2, Height / 4));
        const Color Base = Spines[(Roll >> 20) % Spines.size()];
        for (int Column = 0; Column < Wide; Column++) {
            for (int Row = 0; Row < High; Row++) {
                const int Y = Plank - 1 - Row;
                float Level = 0.82f + 0.2f * Grain(X + Column, Y, 43);
                if (Column == 0) Level *= 1.28f;
                if (Column == Wide - 1 && Wide > 1) Level *= 0.66f;
                if (Row == High - 1) Level *= 1.35f;
                Color Here = Toned(Base, Level);
                if (Row == High / 3 || (Row == High / 3 + 1 && High > 8)) Here = Mixed(Here, Color::Rgb(0xE8, 0xC8, 0x70), 150);
                if (Row == High * 2 / 3 && Wide > 2) Here = Mixed(Here, Color::Rgb(0xE8, 0xC8, 0x70), 110);
                Out.Put(X + Column, Y, Here);
            }
        }
        X += Wide;
    }
}

}  // namespace

void PaintBooks(Canvas &Out, const Scene &Where)
{
    const Box Shelf = Where.Books;
    const Color Wood = Color::Rgb(0x6A, 0x44, 0x26);
    Out.Fill(Shelf.X, Shelf.Y, Shelf.Width, Shelf.Height, Toned(Wood, 0.32f));
    const int Gap = Shelf.Height / Where.BookRows;
    for (int Row = 0; Row < Where.BookRows; Row++) {
        const int Plank = Shelf.Y + (Row + 1) * Gap - 1;
        PaintBookRow(Out, Shelf.X + 2, Shelf.X + Shelf.Width - 2, Plank, Gap - 1, static_cast<std::uint32_t>(Row));
        for (int X = Shelf.X; X < Shelf.X + Shelf.Width; X++) {
            Out.Put(X, Plank, Toned(Wood, 0.95f + 0.2f * Grain(X, Row, 44)));
            Out.Put(X, Plank + 1, Toned(Wood, 0.45f));
        }
    }
    for (int Y = Shelf.Y; Y < Shelf.Y + Shelf.Height; Y++) {
        Out.Put(Shelf.X, Y, Toned(Wood, 1.10f));
        Out.Put(Shelf.X + 1, Y, Toned(Wood, 0.80f));
        Out.Put(Shelf.X + Shelf.Width - 2, Y, Toned(Wood, 0.60f));
        Out.Put(Shelf.X + Shelf.Width - 1, Y, Toned(Wood, 0.40f));
    }
    for (int X = Shelf.X - 1; X <= Shelf.X + Shelf.Width; X++) {
        Out.Put(X, Shelf.Y - 1, Toned(Wood, 1.25f));
        Out.Put(X, Shelf.Y, Toned(Wood, 0.9f));
    }
    // A cobweb in the corner, and the perch the owl sits on.
    for (int Step = 0; Step < std::max(3, static_cast<int>(Where.Unit * 8.0f)); Step++) {
        Out.Put(Shelf.X + 2 + Step, Shelf.Y + 1 + Step / 2, Color::Rgb(0x90, 0x90, 0x98));
        Out.Put(Shelf.X + 2 + Step / 2, Shelf.Y + 1 + Step, Color::Rgb(0x70, 0x70, 0x78));
    }
    const int Perch = std::max(4, static_cast<int>(Where.Unit * 12.0f));
    for (int X = Where.OwlX - Perch / 2; X < Where.OwlX + Perch / 2; X++) {
        Out.Put(X, Where.OwlY, Toned(Wood, 1.0f));
        Out.Put(X, Where.OwlY + 1, Toned(Wood, 0.5f));
    }
}

bool InGlass(const Box &Window, int X, int Y)
{
    const int Left = Window.X + 2, Right = Window.X + Window.Width - 3;
    const int Top = Window.Y + 2, Bottom = Window.Y + Window.Height - 3;
    if (X < Left || X > Right || Y < Top || Y > Bottom) return false;
    const float Radius = static_cast<float>(Right - Left + 1) * 0.5f;
    const float Dy = static_cast<float>(Y) + 0.5f - (static_cast<float>(Top) + Radius);
    if (Dy >= 0.0f) return true;
    const float Dx = static_cast<float>(X) + 0.5f - (static_cast<float>(Left) + Radius);
    return Dx * Dx + Dy * Dy <= Radius * Radius;
}

void PaintWindowFrame(Canvas &Out, const Scene &Where, Color Stone)
{
    const Box W = Where.Window;
    const float Radius = static_cast<float>(W.Width) * 0.5f;
    for (int Y = W.Y - 1; Y < W.Y + W.Height; Y++) {
        for (int X = W.X - 1; X < W.X + W.Width + 1; X++) {
            const float Dy = static_cast<float>(Y) + 0.5f - (static_cast<float>(W.Y) + Radius);
            const float Dx = static_cast<float>(X) + 0.5f - (static_cast<float>(W.X) + Radius);
            const bool Round = Dy < 0.0f;
            const bool Inside = !Round ? X >= W.X && X < W.X + W.Width : Dx * Dx + Dy * Dy <= (Radius + 1.0f) * (Radius + 1.0f);
            if (!Inside || InGlass(W, X, Y)) continue;
            float Level = 1.05f + 0.25f * Grain(X, Y, 51);
            if (X <= W.X || Y <= W.Y) Level *= 1.2f;
            if (X >= W.X + W.Width - 1) Level *= 0.7f;
            Out.Put(X, Y, Toned(Stone, Level));
        }
    }
    // The sill, with the shade it throws on the wall below.
    Out.Fill(W.X - 2, W.Y + W.Height, W.Width + 4, 2, Toned(Stone, 1.15f));
    Out.Fill(W.X - 2, W.Y + W.Height + 2, W.Width + 4, 1, Toned(Stone, 0.40f));
    Out.Fill(W.X - 2, W.Y + W.Height, W.Width + 4, 1, Toned(Stone, 1.40f));
}

void PaintJars(Canvas &Out, const Scene &Where)
{
    const Box Shelf = Where.Jars;
    const Color Wood = Color::Rgb(0x6A, 0x44, 0x26);
    const int   Wide = std::max(4, static_cast<int>(Where.Unit * 8.0f));
    const int   Plank = Shelf.Y + Shelf.Height;
    int         X = Shelf.X + 1;
    for (int Index = 0; X + Wide < Shelf.X + Shelf.Width; Index++) {
        const std::uint32_t Roll = Hash(Index, 7, 61);
        const bool Tall = Roll % 3u == 0;
        const int High = Tall ? Shelf.Height : Shelf.Height * 3 / 4;
        const Color Stuff = Color::FromRaw(Ingredients()[Roll >> 4 & 15u].Color);
        for (int Row = 0; Row < High; Row++) {
            for (int Column = 0; Column < Wide; Column++) {
                const int Px = X + Column, Py = Plank - 1 - Row;
                const bool Rim = Column == 0 || Column == Wide - 1 || Row == 0;
                if (Row >= High - 2) { Out.Put(Px, Py, Toned(Wood, 1.0f + 0.2f * (Column == 0))); continue; }
                Color Here = Rim ? Mixed(Stuff, Color::Rgb(0x30, 0x34, 0x40), 170) : Toned(Stuff, 0.78f + 0.25f * Grain(Px, Py, 62));
                if (Column == 1 && !Rim) Here = Lighter(Here, 70);
                if (Row == High - 3 && !Rim) Here = Lighter(Here, 55);
                Out.Put(Px, Py, Here);
            }
        }
        if (Wide >= 6) Out.Fill(X + Wide / 2 - 1, Plank - High / 2 - 1, 2, 2, Color::Rgb(0xE8, 0xDC, 0xB8));
        X += Wide + 1 + static_cast<int>(Roll >> 12) % 2;
    }
    for (int Column = Shelf.X - 1; Column <= Shelf.X + Shelf.Width; Column++) {
        Out.Put(Column, Plank, Toned(Wood, 1.2f));
        Out.Put(Column, Plank + 1, Toned(Wood, 0.55f));
    }
    for (int Down = 0; Down < 3; Down++) {
        Out.Put(Shelf.X + 2, Plank + 2 + Down, Toned(Wood, 0.5f));
        Out.Put(Shelf.X + Shelf.Width - 3, Plank + 2 + Down, Toned(Wood, 0.5f));
    }
}

int FlameRow(const Scene &Where, int Which)
{
    return Where.CandleY[Which] - StandHeight(Where, Which) - WaxHeight(Where, Which) - 1;
}

void PaintStands(Canvas &Out, const Scene &Where)
{
    const Color Brass = Color::Rgb(0xC0, 0x92, 0x3C), Wax = Color::Rgb(0xE8, 0xDC, 0xB8);
    for (int Which = 0; Which < 3; Which++) {
        const int X = Where.CandleX[Which], Base = Where.CandleY[Which];
        const int Stand = StandHeight(Where, Which), Candle = WaxHeight(Where, Which);
        for (int Up = 0; Up < Stand; Up++) Out.Put(X, Base - Up, Toned(Brass, 0.7f + 0.3f * (Up % 3 == 0)));
        if (Stand > 0) {
            Out.Fill(X - 2, Base, 5, 1, Toned(Brass, 0.9f));
            Out.Fill(X - 1, Base - 1, 3, 1, Toned(Brass, 1.1f));
            Out.Fill(X - 2, Base - Stand, 5, 1, Toned(Brass, 1.15f));
        }
        const int Top = Base - Stand;
        for (int Up = 1; Up <= Candle; Up++) {
            Out.Put(X - 1, Top - Up, Toned(Wax, 1.05f));
            Out.Put(X, Top - Up, Toned(Wax, 0.78f));
        }
        Out.Put(X - 1, Top - Candle / 2, Toned(Wax, 0.6f));
    }
}

}  // namespace wizard
