/*
 * items.cpp — a program's items as objects. See painter/gui/items.h.
 */

#include "painter/gui/items.h"

#include <utility>

#include "painter/gui/frame.h"

namespace painter::gui {

namespace detail {

std::uint32_t Numbers::Take()
{
    if (!Free.empty()) {
        const std::uint32_t Number = Free.back();
        Free.pop_back();
        return Number;
    }
    return Next++;
}

void Numbers::Made(std::uint32_t Number, std::uint32_t Parent)
{
    if (Parent == ITEM_AREA) return;
    ParentOf[Number] = Parent;
    In[Parent].push_back(Number);
}

void Numbers::Give(std::uint32_t Number) noexcept
{
    /* What lies in it went with it, all the way down. */
    std::vector<std::uint32_t> Walk{Number};
    while (!Walk.empty()) {
        const std::uint32_t Group = Walk.back();
        Walk.pop_back();
        const auto Inside = In.find(Group);
        if (Inside == In.end()) continue;
        for (std::uint32_t Each : Inside->second) {
            ParentOf.erase(Each);
            try {
                Went.insert(Each);
                Walk.push_back(Each);
            } catch (...) {
                // No room to know it gone: its object says it goes, and the
                // display says it knows no such item — nothing worse.
            }
        }
        In.erase(Inside);
    }
    if (const auto Up = ParentOf.find(Number); Up != ParentOf.end()) {
        if (const auto Siblings = In.find(Up->second); Siblings != In.end())
            std::erase(Siblings->second, Number);
        ParentOf.erase(Up);
    }
    Went.erase(Number);
    try {
        Free.push_back(Number);
    } catch (...) {
        // No room to keep it: the number is let go, and a new one taken next.
    }
}

Made Make(const std::shared_ptr<Anchor> &Where, std::uint32_t Parent, gui::Kind Kind, gui::Role Role)
{
    if (!Where) return {};
    if (Frame *Describing = Where->Describing) return Made{Describing->Describe(Kind, Role, Parent), false};
    if (!Where->Into || !Where->Pool) return {};
    const std::uint32_t Number = Where->Pool->Take();
    Where->Into->Create(Number, Kind, Parent, ITEM_TOP, Role);
    Where->Pool->Made(Number, Parent);
    return Made{Number, true};
}

}  // namespace detail

/* ---- Item ------------------------------------------------------------------ */

Item::Item(std::shared_ptr<detail::Anchor> WhereValue, detail::Made Made) noexcept
    : Where(std::move(WhereValue)), Id(Made.Number), Owns(Made.Owns)
{
    if (Id == 0) Where.reset();
}

Item::Item(Item &&Other) noexcept : Where(std::move(Other.Where)), Id(Other.Id), Owns(Other.Owns)
{
    Other.Id     = 0;
    Other.Owns   = false;
}

Item &Item::operator=(Item &&Other) noexcept
{
    if (this == &Other) return *this;
    Remove();
    Where        = std::move(Other.Where);
    Id           = Other.Id;
    Owns         = Other.Owns;
    Other.Id     = 0;
    Other.Owns   = false;
    return *this;
}

Item::~Item() { Remove(); }

Item::operator bool() const noexcept { return Id != 0 && Where && (Where->Into || Where->Describing); }

void Item::Remove()
{
    if (Owns && Where && Where->Into && Where->Pool) {
        /* Gone with its group already: the display took it then. */
        if (!Where->Pool->Gone(Id)) Where->Into->Remove(Id);
        Where->Pool->Give(Id);
    }
    Where.reset();
    Id   = 0;
    Owns = false;
}

Changes *Item::Into() const noexcept
{
    if (!Where || Id == 0) return nullptr;
    if (Where->Describing) return &Where->Describing->Said;
    return Where->Into;
}

void Item::KeyAs(std::uint64_t Key) noexcept
{
    if (Where && Where->Describing && Id != 0) Where->Describing->Keyed(Id, Key);
}

}  // namespace painter::gui
