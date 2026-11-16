/* typecase stand — BoxOS's own characters held to symbol_source.h: Duppo's
 * two halves drawn as whole cells at every size; large, each part of the
 * one-colour mark in its tone where the master puts it — the body, the line
 * round the eye, the white, the pupil, the spark, the bubbles and the paper
 * round the medium one; small, the eye without the line. */

#include <cstdint>

#include <algorithm>
#include <string>
#include <vector>

#include "boxos_mascot.h"
#include "boxos_symbols.h"
#include "typecase/symbol_source.h"
#include "typecase_stand.h"

namespace stand {

namespace {

/* A pair of cells Width by Height, drawn: the left half's pixels then the
 * right half's, row by row, as one picture twice as wide. */
struct Pair {
    std::int32_t              Width = 0, Height = 0;
    std::vector<std::uint8_t> Coverage;
    bool                      Whole = true;

    std::uint8_t At(std::int32_t X, std::int32_t Y) const
    {
        return Coverage[static_cast<std::size_t>(Y * 2 * Width + X)];
    }
};

Pair DrawPair(typecase::SymbolSource &Symbols, std::int32_t Width, std::int32_t Height)
{
    const typecase::Size At{0, Width, Height, Height * 3 / 4};
    Pair Out{Width, Height, std::vector<std::uint8_t>(static_cast<std::size_t>(2 * Width * Height), 0), true};
    for (int Side = 0; Side < 2; Side++) {
        const auto Half = Symbols.Draw(Side ? BOXOS_SYMBOL_DUPPO_RIGHT : BOXOS_SYMBOL_DUPPO_LEFT, At);
        Out.Whole = Out.Whole && Half && Half->Left == 0 && Half->Top == -At.Baseline && Half->Width == Width &&
                    Half->Height == Height && Half->Coverage.size() == static_cast<std::size_t>(Width * Height);
        if (!Out.Whole) return Out;
        for (std::int32_t Y = 0; Y < Height; Y++)
            for (std::int32_t X = 0; X < Width; X++)
                Out.Coverage[static_cast<std::size_t>(Y * 2 * Width + Side * Width + X)] =
                    Half->Coverage[static_cast<std::size_t>(Y * Width + X)];
    }
    return Out;
}

/* The pixel a point of the master falls in, the mark's ink fitted to the
 * pair as large as it goes and in the middle. */
std::uint8_t Sample(const Pair &Drawn, float MasterX, float MasterY)
{
    const BoxDuppoDisc Inked[] = {BoxDuppoBody, BoxDuppoMarkMoon, BoxDuppoMoons[1], BoxDuppoMoons[2]};
    float Left = 1e9f, Top = 1e9f, Right = -1e9f, Bottom = -1e9f;
    for (const BoxDuppoDisc &Each : Inked) {
        Left = std::min(Left, Each.X - Each.Radius), Right = std::max(Right, Each.X + Each.Radius);
        Top = std::min(Top, Each.Y - Each.Radius), Bottom = std::max(Bottom, Each.Y + Each.Radius);
    }
    const float Across = static_cast<float>(2 * Drawn.Width), Down = static_cast<float>(Drawn.Height);
    const float Scale  = std::min(Across / (Right - Left), Down / (Bottom - Top));
    const float X      = (Across - (Right - Left) * Scale) / 2 + (MasterX - Left) * Scale;
    const float Y      = (Down - (Bottom - Top) * Scale) / 2 + (MasterY - Top) * Scale;
    return Drawn.At(static_cast<std::int32_t>(X), static_cast<std::int32_t>(Y));
}

struct Part {
    const char *Name;
    float       X, Y;
    bool        Ink;
};

}  // namespace

void CheckSymbols(Tally &Count)
{
    typecase::SymbolSource Symbols;
    Count.Check(Symbols.Has(BOXOS_SYMBOL_DUPPO_LEFT) && Symbols.Has(BOXOS_SYMBOL_DUPPO_RIGHT) &&
                    !Symbols.Has(BOXOS_SYMBOL_DUPPO_RIGHT + 1) && !Symbols.Has(BOXOS_SYMBOL_DUPPO_LEFT - 1) &&
                    !Symbols.Has(U'A') && !Symbols.Has(U'─'),
                "the symbols are Duppo's two halves, U+10B000 and U+10B001, and nothing else");

    bool Whole = true, Both = true;
    for (const auto &[Wide, Tall] : {std::pair{5, 10}, {7, 16}, {8, 16}, {9, 20}, {11, 24}, {12, 27}, {18, 40},
                                     {24, 48}, {44, 96}}) {
        const Pair Drawn = DrawPair(Symbols, Wide, Tall);
        Whole            = Whole && Drawn.Whole;
        if (!Drawn.Whole) break;
        int Left = 0, Right = 0;
        for (std::int32_t Y = 0; Y < Tall; Y++)
            for (std::int32_t X = 0; X < Wide; X++) {
                Left += Drawn.At(X, Y) > 128;
                Right += Drawn.At(Wide + X, Y) > 128;
            }
        Both = Both && Left > 0 && Right > 0;
    }
    Count.Check(Whole, "each half drawn as the whole cell, at nine sizes from 5 by 10 to 44 by 96");
    Count.Check(Both, "the mark crosses from one half into the other: ink in both at every size");

    /* Points of the master well inside each part: the body clear of the eye
     * and the bubbles; the middle of the line round the eye; the middle of
     * the white ring; the pupil below and right of the spark; the spark, the
     * bubbles; the paper between the medium bubble and the body. */
    const float LineMiddle  = (BoxDuppoSocket.Radius + BoxDuppoWhite.Radius) / 2;
    const float WhiteMiddle = (BoxDuppoWhite.Radius + BoxDuppoPupil.ReachX) / 2;
    const float Away        = (BoxDuppoMarkMoon.Radius + BoxDuppoMarkGap.Radius) / 2 + 0.14f;
    const Part  Parts[]     = {
        {"the body", BoxDuppoBody.X - 13, BoxDuppoBody.Y + 6, true},
        {"the line round the eye", BoxDuppoSocket.X - LineMiddle, BoxDuppoSocket.Y, false},
        {"the white", BoxDuppoWhite.X - WhiteMiddle, BoxDuppoWhite.Y, true},
        {"the pupil", BoxDuppoPupil.X + 3, BoxDuppoPupil.Y + 3, false},
        {"the spark", BoxDuppoMarkSpark.X, BoxDuppoMarkSpark.Y, true},
        {"the medium bubble", BoxDuppoMarkMoon.X, BoxDuppoMarkMoon.Y, true},
        {"the small bubble", BoxDuppoMoons[1].X, BoxDuppoMoons[1].Y, true},
        {"the tiny bubble", BoxDuppoMoons[2].X, BoxDuppoMoons[2].Y, true},
        {"the paper round the medium bubble", BoxDuppoMarkMoon.X - Away * 0.7071f, BoxDuppoMarkMoon.Y - Away * 0.7071f, false},
        {"the paper beside the body", BoxDuppoBody.X - 17.0f, BoxDuppoBody.Y + 14.0f, false},
    };
    bool        Toned = true;
    std::string Wrong;
    for (const auto &[Wide, Tall] : {std::pair{44, 96}, {64, 128}}) {
        const Pair Drawn = DrawPair(Symbols, Wide, Tall);
        for (const Part &Each : Parts) {
            const std::uint8_t Shown = Sample(Drawn, Each.X, Each.Y);
            if (Each.Ink ? Shown >= 200 : Shown <= 55) continue;
            Toned = false;
            Wrong += std::string(" ") + Each.Name + " at " + std::to_string(Wide) + "x" + std::to_string(Tall) + " (" +
                     std::to_string(Shown) + ")";
        }
    }
    Count.Check(Toned, "large, each part of the mark in its tone where the master puts it — body, line round the "
                       "eye, white, pupil, spark, bubbles, the paper round the medium one" + Wrong);

    const Pair Small = DrawPair(Symbols, 11, 24);
    Count.Check(Sample(Small, Parts[1].X, Parts[1].Y) >= 200 && Sample(Small, Parts[3].X, Parts[3].Y) <= 55,
                "small, at 11 by 24, the line round the eye is not drawn — the body runs on to the pupil, and the "
                "pupil is the paper");
}

}  // namespace stand
