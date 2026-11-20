/*
 * frame.cpp — a frame described whole, and only what differs from the frame
 * before sent. See painter/gui/frame.h.
 */

#include "painter/gui/frame.h"

#include <algorithm>
#include <span>
#include <utility>


namespace painter::gui {

Frame::Frame() : Anchored(std::make_shared<detail::Anchor>())
{
    Anchored->Describing = this;
    Said.RememberOnly();
}

Frame::~Frame()
{
    /* Its objects say nothing once it is drawn. */
    Anchored->Describing = nullptr;
}

std::uint32_t Frame::Describe(gui::Kind Kind, gui::Role Role, std::uint32_t Parent)
{
    List.push_back(Described{Kind, Role, Parent, Placed[Parent]++, false});
    return static_cast<std::uint32_t>(List.size());
}

void Frame::Keyed(std::uint32_t Number, std::uint64_t Key) noexcept
{
    if (Number == 0 || Number > List.size()) return;
    List[Number - 1].Key   = Key;
    List[Number - 1].Keyed = true;
}

namespace detail {

Frame &Drawer::Begin()
{
    Describing.reset(new Frame());
    return *Describing;
}

void Drawer::End()
{
    if (!Describing) return;
    const std::unique_ptr<Frame> Now = std::move(Describing);
    if (!Where || !Where->Into || !Where->Pool) return;
    Changes &Into = *Where->Into;
    Numbers &Pool = *Where->Pool;

    /* Last frame's items by what they are found by, each the first of its
     * kind found by halving. */
    Last.clear();
    for (std::uint32_t I = 0; I < Items.size(); I++) {
        Items[I].Found = false;
        Last.emplace_back(Items[I].By, I);
    }
    std::sort(Last.begin(), Last.end());

    const std::size_t Count = Now->List.size();
    Numbered.assign(Count + 1, ITEM_AREA);
    Under.assign(Count + 1, 0);   // the item laid last under each, by its number in this frame
    Drawn.clear();
    Drawn.reserve(Count);
    for (std::size_t Index = 0; Index < Count; Index++) {
        const Frame::Described &Each   = Now->List[Index];
        const std::uint32_t     Parent = Numbered[Each.Parent];
        const Match             By{Parent, static_cast<std::uint8_t>(Each.Kind), Each.Keyed, Each.Key};
        const auto              Before = std::lower_bound(Last.begin(), Last.end(), By,
                                                          [](const std::pair<Match, std::uint32_t> &One, const Match &Of) { return One.first < Of; });
        std::uint32_t Number = 0;
        bool          Again  = false;
        if (Before != Last.end() && Before->first == By && !Items[Before->second].Found) {
            Was &Then  = Items[Before->second];
            Number     = Then.Number;
            Then.Found = true;
            Again      = true;
            if (Then.Role != Each.Role) Into.Role(Number, Each.Role);
        } else {
            /* New: made right above the sibling described before it. */
            Number = Pool.Take();
            Into.Create(Number, Each.Kind, Parent, Under[Each.Parent], Each.Role);
        }
        Numbered[Index + 1] = Number;
        Under[Each.Parent] = Number;
        Drawn.push_back(Was{By, Number, Each.Role, Again, false});

        /* What it says now, said where it differs; what it said and says no
         * more, unsaid — its role is its making's, not a word of the frame's. */
        const auto Here = static_cast<std::uint32_t>(Index + 1);
        Now->Said.EachSaid(Here, [&](std::uint16_t Property, std::span<const std::uint8_t> Value) { Into.Say(Number, Property, Value); });
        Unsaid.clear();
        Into.EachSaid(Number, [&](std::uint16_t Property, std::span<const std::uint8_t>) {
            if (Property != ITEM_PROP_ROLE && !Now->Said.Said(Here, Property)) Unsaid.push_back(Property);
        });
        for (std::uint16_t Property : Unsaid) Into.Reset(Number, Property);
    }

    /* Under a parent whose items found again lie otherwise than they did, all
     * of its items laid again in the order described — the parents in the
     * order of their numbers. */
    const auto ByParent = [](const Placed &A, const Placed &B) { return A.first < B.first; };
    Laid.clear();
    Before.clear();
    for (std::uint32_t I = 0; I < Drawn.size(); I++) Laid.emplace_back(Drawn[I].By.Parent, I);
    for (std::uint32_t I = 0; I < Items.size(); I++) Before.emplace_back(Items[I].By.Parent, I);
    std::stable_sort(Laid.begin(), Laid.end(), ByParent);
    std::stable_sort(Before.begin(), Before.end(), ByParent);
    for (std::size_t L = 0, B = 0; L < Laid.size();) {
        const std::uint32_t Parent = Laid[L].first;
        std::size_t         End    = L;
        while (End < Laid.size() && Laid[End].first == Parent) End++;
        while (B < Before.size() && Before[B].first < Parent) B++;
        std::size_t Ended = B;
        while (Ended < Before.size() && Before[Ended].first == Parent) Ended++;
        /* Those found again, in the order they lay then and lie now. */
        bool        Same = true;
        std::size_t Then = B, Now2 = L;
        for (;;) {
            while (Then < Ended && !Items[Before[Then].second].Found) Then++;
            while (Now2 < End && !Drawn[Laid[Now2].second].Again) Now2++;
            if (Then == Ended || Now2 == End) {
                Same = Then == Ended && Now2 == End;
                break;
            }
            if (Items[Before[Then].second].Number != Drawn[Laid[Now2].second].Number) {
                Same = false;
                break;
            }
            Then++, Now2++;
        }
        if (!Same)
            for (std::size_t At = L; At < End; At++)
                Into.Restack(Drawn[Laid[At].second].Number, Parent, At == L ? 0 : Drawn[Laid[At - 1].second].Number);
        L = End;
        B = Ended;
    }

    /* What is no more, removed — the last described first, so a group's items
     * go before it. */
    for (auto Each = Items.rbegin(); Each != Items.rend(); ++Each)
        if (!Each->Found) {
            Into.Remove(Each->Number);
            Pool.Give(Each->Number);
        }
    std::swap(Items, Drawn);
}

}  // namespace detail

}  // namespace painter::gui
