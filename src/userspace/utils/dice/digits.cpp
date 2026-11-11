/* digits.cpp — the font of the numbers. */

#include "digits.h"

#include <array>

namespace dice {

namespace {

constexpr int Wide = 3;
constexpr int High = 5;
constexpr int Step = Wide + 1;

// Each digit and the minus as five rows of three pixels, the leftmost the high bit.
constexpr std::array<std::array<std::uint8_t, High>, 11> Font = {{
    {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1},
    {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7},
    {0, 0, 7, 0, 0},
}};

}  // namespace

int DigitsWide(std::size_t Length, int Scale)
{
    return Length ? (static_cast<int>(Length) * Step - 1) * Scale : 0;
}

int DigitsHigh(int Scale)
{
    return High * Scale;
}

void DrawDigits(const Canvas &On, painter::core::Point At, std::string_view Text, int Scale, Color Ink)
{
    int Left = At.X;
    for (const char Glyph : Text) {
        const auto &Rows = Font[Glyph == '-' ? 10 : static_cast<std::size_t>(Glyph - '0')];
        for (int Row = 0; Row < High; Row++) {
            for (int Column = 0; Column < Wide; Column++) {
                if (!(Rows[static_cast<std::size_t>(Row)] & (1 << (Wide - 1 - Column)))) continue;
                for (int Dy = 0; Dy < Scale; Dy++)
                    for (int Dx = 0; Dx < Scale; Dx++)
                        On.Plot(Left + Column * Scale + Dx, At.Y + Row * Scale + Dy, Ink);
            }
        }
        Left += Step * Scale;
    }
}

}  // namespace dice
