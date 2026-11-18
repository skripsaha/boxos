/* layout_ask.cpp — what is asked of a text laid: where the caret stands,
 * which character is nearest a point, where a stretch lies, the text to
 * copy. See layout.h. */

#include "typecase/layout.h"

#include <algorithm>
#include <cmath>

#include "characters.h"

namespace typecase {

namespace {

/* Whether the caret stands before character Index of Said: not before one
 * that clings to the character before it — but in a secret text, where
 * every character shows as a dot of its own. */
bool Stands(const std::u32string &Said, bool Secret, std::uint32_t Index) noexcept
{
    if (Index == 0 || Index >= Said.size() || Secret) return true;
    return !Clings(Said[Index]) && Said[Index - 1] != Joiner;
}

}  // namespace

Caret Layout::CaretAt(std::uint32_t Index) const noexcept
{
    if (LineList.empty()) return {};
    Index = std::min(Index, Count());
    while (!Stands(Said, Hidden, Index)) Index--;
    const LaidLine &Line = LineList[LineOf[Index]];
    return Caret{Stops[Index], Line.Top, Line.Top + Line.Height, LineOf[Index]};
}

std::uint32_t Layout::IndexAt(float X, float Y) const noexcept
{
    if (LineList.empty()) return 0;
    std::uint32_t Line = 0;
    while (Line + 1 < LineList.size() && Y >= LineList[Line].Top + LineList[Line].Height) Line++;
    const LaidLine     &On   = LineList[Line];
    const std::uint32_t Last = Line + 1 < LineList.size() ? std::max(On.To, On.From + 1) - 1 : Count();
    std::uint32_t       Best = On.From;
    float               Near = -1;
    for (std::uint32_t K = On.From; K <= Last && K <= Count(); K++) {
        if (LineOf[K] != Line || !Stands(Said, Hidden, K)) continue;
        const float Far = std::fabs(Stops[K] - X);
        if (Near < 0 || Far < Near) Best = K, Near = Far;
    }
    return Best;
}

std::vector<Area> Layout::Cover(std::uint32_t From, std::uint32_t To) const
{
    std::vector<Area> Lying;
    From = std::min(From, Count());
    To   = std::min(To, Count());
    for (std::size_t L = 0; L < LineList.size() && From < To; L++) {
        const LaidLine     &Line = LineList[L];
        const std::uint32_t A = std::max(From, Line.From), B = std::min(To, Line.To);
        if (A >= B) continue;
        const float Left  = Stops[A];
        const float Right = B == Line.To ? Line.Right : Stops[B];
        Lying.push_back(Area{Left, Line.Top, std::max(Right - Left, 0.0f), Line.Height});
    }
    return Lying;
}

std::uint32_t Layout::Next(std::uint32_t Index) const noexcept
{
    if (Index >= Count()) return Count();
    do Index++;
    while (!Stands(Said, Hidden, Index));
    return Index;
}

std::uint32_t Layout::Previous(std::uint32_t Index) const noexcept
{
    Index = std::min(Index, Count());
    if (Index == 0) return 0;
    do Index--;
    while (!Stands(Said, Hidden, Index));
    return Index;
}

std::string Layout::Copy(std::uint32_t From, std::uint32_t To) const
{
    if (Hidden) return {};
    From = std::min(From, Count());
    To   = std::min(To, Count());
    return From < To ? Utf8Of(std::u32string_view(Said).substr(From, To - From)) : std::string();
}

Size MissingCell(char32_t Character, float Em) noexcept
{
    const float Wide = Character > 0xFFFF ? 0.85f : 0.6f;
    return Size{Em, std::max<std::int32_t>(1, static_cast<std::int32_t>(std::lround(Em * Wide))),
                std::max<std::int32_t>(1, static_cast<std::int32_t>(std::lround(Em))),
                static_cast<std::int32_t>(std::lround(Em * 0.8f))};
}

float MissingAdvance(char32_t Character, float Em) noexcept
{
    return static_cast<float>(MissingCell(Character, Em).Width) + std::max(1.0f, std::round(Em / 10));
}

}  // namespace typecase
