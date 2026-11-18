/*
 * frame.cpp — a frame described whole, and only what differs from the frame
 * before sent. See painter/gui/frame.h.
 */

#include "painter/gui/frame.h"

#include <algorithm>
#include <map>
#include <set>
#include <utility>


namespace painter::gui {

Frame::Frame() : Anchored(std::make_shared<detail::Anchor>()) { Anchored->Describing = this; }

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

    /* Last frame's items as each is found again, and its order under each
     * parent. */
    std::map<Match, const Was *>                        Last;
    std::map<std::uint32_t, std::vector<std::uint32_t>> LastOrder;
    for (const Was &Each : Items) {
        Last.emplace(Each.By, &Each);
        LastOrder[Each.By.Parent].push_back(Each.Number);
    }

    std::vector<std::uint32_t>                          Numbers2(Now->List.size() + 1, ITEM_AREA);
    std::set<std::uint32_t>                             Found;     // last frame's items found again
    std::map<std::uint32_t, std::vector<std::uint32_t>> Order;     // this frame's, under each parent
    std::vector<Was>                                    Drawn;
    Drawn.reserve(Now->List.size());
    for (std::size_t Index = 0; Index < Now->List.size(); Index++) {
        const Frame::Described &Each   = Now->List[Index];
        const std::uint32_t     Parent = Numbers2[Each.Parent];
        const Match             By{Parent, static_cast<std::uint8_t>(Each.Kind), Each.Keyed, Each.Key};
        const auto              Before = Last.find(By);
        std::uint32_t           Number = 0;
        if (Before != Last.end() && !Found.contains(Before->second->Number)) {
            Number = Before->second->Number;
            Found.insert(Number);
            if (Before->second->Role != Each.Role) Into.Role(Number, Each.Role);
        } else {
            /* New: made right above the sibling described before it. */
            Number = Pool.Take();
            Into.Create(Number, Each.Kind, Parent, Order[Parent].empty() ? 0 : Order[Parent].back(), Each.Role);
        }
        Numbers2[Index + 1] = Number;
        Order[Parent].push_back(Number);
        Drawn.push_back(Was{By, Number, Each.Role});

        /* What it says now, said where it differs; what it said and says no
         * more, unsaid — its role is its making's, not a word of the frame's. */
        const auto Here = static_cast<std::uint32_t>(Index + 1);
        for (std::uint16_t Property : Now->Said.SaidOf(Here)) Into.Say(Number, Property, *Now->Said.Said(Here, Property));
        for (std::uint16_t Property : Into.SaidOf(Number))
            if (Property != ITEM_PROP_ROLE && !Now->Said.Said(Here, Property)) Into.Reset(Number, Property);
    }

    /* Under a parent whose items found again lie otherwise than they did, all
     * of its items laid again in the order described. */
    for (const auto &[Parent, Laid] : Order) {
        std::vector<std::uint32_t> Then, Kept;
        for (std::uint32_t Number : LastOrder[Parent])
            if (Found.contains(Number)) Then.push_back(Number);
        for (std::uint32_t Number : Laid)
            if (Found.contains(Number)) Kept.push_back(Number);
        if (Then == Kept) continue;
        for (std::size_t At = 0; At < Laid.size(); At++) Into.Restack(Laid[At], Parent, At == 0 ? 0 : Laid[At - 1]);
    }

    /* What is no more, removed — the last described first, so a group's items
     * go before it. */
    for (auto Each = Items.rbegin(); Each != Items.rend(); ++Each)
        if (!Found.contains(Each->Number)) {
            Into.Remove(Each->Number);
            Pool.Give(Each->Number);
        }
    Items = std::move(Drawn);
}

}  // namespace detail

}  // namespace painter::gui
