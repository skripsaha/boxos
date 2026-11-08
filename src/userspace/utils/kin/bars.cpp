/* The tabs across the top and the status line at the foot. */

#include "bars.h"

#include <algorithm>
#include <string>

#include "painter/tui/glyph.h"

#include "glyph.h"

namespace kin {

namespace {

/* A shortened tab name keeps five letters before its "...": fewer, and the
 * tabs of a project's files all read alike. */
constexpr std::int32_t LeastName = 8;

std::int32_t DigitsOf(std::size_t Number)
{
    std::int32_t Digits = 1;
    for (; Number >= 10; Number /= 10) Digits++;
    return Digits;
}

}  // namespace

/* The whole row is the tabs': the machine's notices — the clock, the pulse —
 * are layers of their own above it, and stand over what they cover. */
void PaintTabs(tui::Frame &Out, std::int32_t Room, std::span<const TabFace> Tabs, std::size_t Current)
{
    Out.Fill({0, 0, Room, 1}, tui::Cell{' ', BarInk, BarPaper});
    const std::size_t Count = Tabs.size();
    if (Count == 0 || Room <= 2) return;

    auto Fixed = [&](std::size_t Index) { return 3 + DigitsOf(Index + 1) + (Tabs[Index].Dirty ? 2 : 0); };
    std::int32_t Longest = 0;
    for (const TabFace &One : Tabs) Longest = std::max(Longest, static_cast<std::int32_t>(CellsOf(One.Name)));
    auto WidthOf = [&](std::size_t Index, std::int32_t Cap) {
        return Fixed(Index) + std::min(Cap, static_cast<std::int32_t>(CellsOf(Tabs[Index].Name)));
    };
    auto Total = [&](std::int32_t Cap) {
        std::int32_t Sum = 0;
        for (std::size_t Index = 0; Index < Count; Index++) Sum += WidthOf(Index, Cap);
        return Sum;
    };

    std::int32_t Cap = Longest;
    while (Cap > LeastName && Total(Cap) > Room) Cap--;

    /* The tab in front ends in the left half of the row: the right of it is
     * where the machine's notices stand, in layers above this one, and the
     * tab in front is the one that must not be under them. When the tabs do
     * not all fit, or that one would end further right, the row shows a
     * stretch of them: before the front tab as many as leave it in the left
     * half, after it as many as the row takes; arrows at the ends say there
     * are more. */
    std::int32_t FrontEnds = 0;
    for (std::size_t Index = 0; Index <= Current; Index++) FrontEnds += WidthOf(Index, Cap);
    std::size_t First = 0, Last = Count;
    if (Total(Cap) > Room || FrontEnds > Room / 2) {
        First = Current;
        Last  = Current + 1;
        std::int32_t Used = 1 + WidthOf(Current, Cap);
        while (First > 0 && Used + WidthOf(First - 1, Cap) <= Room / 2) Used += WidthOf(--First, Cap);
        while (Last < Count && Used + WidthOf(Last, Cap) + 1 <= Room) Used += WidthOf(Last++, Cap);
    }

    std::int32_t X = 0;
    if (First > 0) (void)Out.Put({X++, 0}, MoreLeft, {HintInk, BarPaper});
    for (std::size_t Index = First; Index < Last; Index++) {
        const TabFace     &One   = Tabs[Index];
        const bool         Front = Index == Current;
        const core::Color  Under = Front ? ShownTabPaper : BarPaper;
        const std::int32_t Wide  = WidthOf(Index, Cap);
        Out.Fill({X, 0, Wide, 1}, tui::Cell{' ', BarInk, Under});

        std::int32_t Col = X + 1;
        Col += PutText(Out, {Col, 0}, std::to_string(Index + 1), Wide,
                       One.Warned ? TroubleInk : Front ? TabNumberInk : HintInk, Under) + 1;
        Col += PutText(Out, {Col, 0}, Shortened(One.Name, static_cast<std::size_t>(Cap)), Wide,
                       Front ? ShownTabInk : BarInk, Under);
        if (One.Dirty) (void)Out.Put({Col + 1, 0}, tui::Glyphs::Bullet, {DirtyInk, Under});
        X += Wide;
        if (!Front && Index + 1 < Last && Index + 1 != Current) (void)Out.Put({X - 1, 0}, RuleDown, {RuleInk, BarPaper});
    }
    if (Last < Count) (void)Out.Put({X, 0}, MoreRight, {HintInk, BarPaper});
}

StatusLine::StatusLine(tui::Frame &OutValue, core::Size Screen, std::string_view Said, bool Trouble,
                       std::string_view Hint)
    : Out(OutValue), Y(Screen.Height - 1)
{
    Out.Fill({0, Y, Screen.Width, 1}, tui::Cell{' ', BarInk, BarPaper});
    const std::string_view Right = Said.empty() ? Hint : Said;
    const core::Color      Ink   = Said.empty() ? HintInk : Trouble ? TroubleInk : GoodInk;
    const std::int32_t     Cells = std::min(static_cast<std::int32_t>(CellsOf(Right)), Screen.Width - 2);
    PutText(Out, {Screen.Width - 1 - Cells, Y}, Shortened(Right, static_cast<std::size_t>(std::max(Cells, 0))), Cells,
            Ink, BarPaper);
    End = Screen.Width - 1 - Cells - 2;
}

void StatusLine::Put(std::string_view Text, core::Color Ink, core::Color Under)
{
    if (X >= End) return;
    X += PutText(Out, {X, Y}, Text, End - X, Ink, Under) + 1;
}

void StatusLine::Badge(std::string_view Word, core::Color Under)
{
    Put(std::string(" ") + std::string(Word) + " ", BadgeInk, Under);
}

}  // namespace kin
