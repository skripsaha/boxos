/*
 * item_tree.cpp — the items of one area as its program said them. See
 * item_tree.h.
 */

#include "item_tree.h"

#include <algorithm>
#include <utility>

namespace items {

namespace {

// What changing Property asks of the drawing: the item's picture drawn again,
// or only where it lies.
struct Asks {
    bool Picture;
    bool Place;
};

Asks AsksOf(std::uint16_t Property) noexcept
{
    switch (Property) {
    case ITEM_PROP_PLACE:
    case ITEM_PROP_OPACITY:
    case ITEM_PROP_HIDDEN:
    case ITEM_PROP_TURN:
    case ITEM_PROP_SCALE:
    case ITEM_PROP_ANCHOR:
    case ITEM_PROP_CLIP:
        return {false, true};
    case ITEM_PROP_SIZE:
        return {true, true};
    default:
        return {true, false};
    }
}

}  // namespace

std::vector<std::uint32_t> BlocksOf(const Properties &Of)
{
    std::vector<std::uint32_t> Blocks;
    if (Of.Picture) Blocks.push_back(Of.Picture->Block);
    if (Of.Canvas) Blocks.push_back(*Of.Canvas);
    if (Of.Fills)
        for (const Fill &Each : *Of.Fills)
            if (Each.Kind == ITEM_FILL_PICTURE) Blocks.push_back(Each.Block);
    if (Of.Stroke && Of.Stroke->With.Kind == ITEM_FILL_PICTURE) Blocks.push_back(Of.Stroke->With.Block);
    return Blocks;
}

Tree::~Tree()
{
    for (const auto &[Id, Each] : All) Held.TextBytes -= TextBytes(Each.Said);
    Held.Items -= static_cast<std::uint32_t>(All.size());
}

Item *Tree::Find(std::uint32_t Id) noexcept
{
    const auto Found = All.find(Id);
    return Found == All.end() ? nullptr : &Found->second;
}

const Item *Tree::Find(std::uint32_t Id) const noexcept
{
    const auto Found = All.find(Id);
    return Found == All.end() ? nullptr : &Found->second;
}

std::vector<std::uint32_t> *Tree::ChildrenOf(std::uint32_t Parent) noexcept
{
    if (Parent == ITEM_AREA) return &Roots;
    Item *Group = Find(Parent);
    return Group && Group->Kind == ITEM_GROUP ? &Group->Children : nullptr;
}

void Tree::MarkUp(std::uint32_t Parent) noexcept
{
    AnyMarked = true;
    while (Parent != ITEM_AREA) {
        Item *Group = Find(Parent);
        if (!Group || Group->Within) return;
        Group->Within = true;
        Parent        = Group->Parent;
    }
}

void Tree::Mark(Item &Of, bool Picture, bool Place) noexcept
{
    Of.Redraw = Of.Redraw || Picture;
    Of.Moved  = Of.Moved || Place;
    Of.Within = true;
    MarkUp(Of.Parent);
}

void Tree::Uses(const Item &Of, bool Add) noexcept
{
    try {
        for (std::uint32_t Block : BlocksOf(Of.Said)) {
            if (Add) {
                BlockUsers[Block].insert(Of.Id);
                continue;
            }
            const auto Found = BlockUsers.find(Block);
            if (Found == BlockUsers.end()) continue;
            Found->second.erase(Of.Id);
            if (Found->second.empty()) BlockUsers.erase(Found);
        }
    } catch (...) {
        // No room to note who shows a block: its changes may go unseen by
        // that item until it is drawn again for another reason.
    }
}

/* Id put among Parent's items above Above, its depth in Depth. */
error_t Tree::Place(std::uint32_t Id, std::uint32_t Parent, std::uint32_t Above, std::uint32_t &Depth) noexcept
{
    std::vector<std::uint32_t> *Siblings = ChildrenOf(Parent);
    if (!Siblings) return ERR_INVALID_ARGUMENT;
    Depth = Parent == ITEM_AREA ? 1 : Find(Parent)->Depth + 1;
    if (Depth > ITEM_DEPTH_MOST) return ERR_OUT_OF_RANGE;
    std::size_t At = 0;
    if (Above == ITEM_TOP) {
        At = Siblings->size();
    } else if (Above != 0) {
        const auto Found = std::find(Siblings->begin(), Siblings->end(), Above);
        if (Found == Siblings->end() || Above == Id) return ERR_INVALID_ARGUMENT;
        At = static_cast<std::size_t>(Found - Siblings->begin()) + 1;
    }
    try {
        Siblings->insert(Siblings->begin() + static_cast<std::ptrdiff_t>(At), Id);
    } catch (...) {
        return ERR_NO_MEMORY;
    }
    return OK;
}

error_t Tree::Create(std::uint32_t Id, const ItemCreate &How) noexcept
{
    if (Id == ITEM_AREA || Id == ITEM_TOP) return ERR_INVALID_ARGUMENT;
    if (How.Kind == 0 || How.Kind >= ITEM_KINDS || How.Role >= ITEM_ROLES) return ERR_INVALID_ARGUMENT;
    if (How.Kind == ITEM_CELLS) return ERR_UNSUPPORTED;
    if (All.count(Id)) return ERR_ALREADY_EXISTS;
    if (Held.Items >= ITEM_ITEMS_MOST) return ERR_QUOTA_EXCEEDED;
    if (How.Parent == Id) return ERR_INVALID_ARGUMENT;
    try {
        Item Made;
        Made.Id     = Id;
        Made.Kind   = How.Kind;
        Made.Role   = How.Role;
        Made.Parent = How.Parent;
        auto [At, Fresh] = All.emplace(Id, std::move(Made));
        (void)Fresh;
        const error_t Placed = Place(Id, How.Parent, How.Above, At->second.Depth);
        if (Placed != OK) {
            All.erase(At);
            return Placed;
        }
        Held.Items++;
        Mark(At->second, true, true);
        return OK;
    } catch (...) {
        All.erase(Id);
        return ERR_NO_MEMORY;
    }
}

void Tree::Forget(Item &Of) noexcept
{
    for (std::uint32_t Child : Of.Children)
        if (Item *Inner = Find(Child)) Forget(*Inner);
    if (!Of.Shown.Empty()) {
        try {
            Gone.push_back(Of.Shown);
        } catch (...) {
            // No room to note where it was: drawn over with the next change
            // that covers it, or the next whole frame.
        }
    }
    Uses(Of, false);
    Held.TextBytes -= TextBytes(Of.Said);
    Held.Items--;
    const std::uint32_t Id = Of.Id;
    All.erase(Id);
}

error_t Tree::Remove(std::uint32_t Id) noexcept
{
    Item *Of = Find(Id);
    if (!Of) return ERR_OBJECT_NOT_FOUND;
    const std::uint32_t         Parent   = Of->Parent;
    std::vector<std::uint32_t> *Siblings = ChildrenOf(Parent);
    if (Siblings) Siblings->erase(std::remove(Siblings->begin(), Siblings->end(), Id), Siblings->end());
    Forget(*Of);
    MarkUp(Parent);
    return OK;
}

error_t Tree::Restack(std::uint32_t Id, const ItemPlaceAmong &How) noexcept
{
    Item *Of = Find(Id);
    if (!Of) return ERR_OBJECT_NOT_FOUND;
    /* Never into itself: walking up from the new parent never meets it. */
    for (std::uint32_t Up = How.Parent; Up != ITEM_AREA;) {
        if (Up == Id) return ERR_INVALID_ARGUMENT;
        const Item *Group = Find(Up);
        if (!Group) return ERR_INVALID_ARGUMENT;
        Up = Group->Parent;
    }
    /* How deep what is in it goes, from it. */
    std::uint32_t Deepest = 0;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> Walk{{Id, 1}};
    try {
        while (!Walk.empty()) {
            const auto [Next, Down] = Walk.back();
            Walk.pop_back();
            Deepest = std::max(Deepest, Down);
            if (const Item *Inner = Find(Next))
                for (std::uint32_t Child : Inner->Children) Walk.push_back({Child, Down + 1});
        }
    } catch (...) {
        return ERR_NO_MEMORY;
    }
    std::vector<std::uint32_t> *Target = ChildrenOf(How.Parent);
    if (!Target) return ERR_INVALID_ARGUMENT;
    const std::uint32_t NewDepth = How.Parent == ITEM_AREA ? 1 : Find(How.Parent)->Depth + 1;
    if (NewDepth + Deepest - 1 > ITEM_DEPTH_MOST) return ERR_OUT_OF_RANGE;
    if (How.Above != 0 && How.Above != ITEM_TOP &&
        (How.Above == Id || std::find(Target->begin(), Target->end(), How.Above) == Target->end()))
        return ERR_INVALID_ARGUMENT;

    const std::uint32_t         Old      = Of->Parent;
    std::vector<std::uint32_t> *Siblings = ChildrenOf(Old);
    if (Siblings) Siblings->erase(std::remove(Siblings->begin(), Siblings->end(), Id), Siblings->end());
    std::uint32_t Depth = 0;
    const error_t Placed = Place(Id, How.Parent, How.Above, Depth);
    if (Placed != OK) {
        /* No room where it goes: back where it was, on top. */
        std::uint32_t Back = 0;
        (void)Place(Id, Old, ITEM_TOP, Back);
        return Placed;
    }
    Of->Parent = How.Parent;
    /* Every depth inside it moved with it. */
    const std::int64_t By = static_cast<std::int64_t>(Depth) - Of->Depth;
    try {
        Walk = {{Id, 0}};
        while (!Walk.empty()) {
            const std::uint32_t Next = Walk.back().first;
            Walk.pop_back();
            if (Item *Inner = Find(Next)) {
                Inner->Depth = static_cast<std::uint32_t>(Inner->Depth + By);
                for (std::uint32_t Child : Inner->Children) Walk.push_back({Child, 0});
            }
        }
    } catch (...) {
    }
    MarkUp(Old);
    Mark(*Of, false, true);
    return OK;
}

error_t Tree::Change(std::uint32_t Id, std::uint16_t Property, std::span<const std::uint8_t> Value) noexcept
{
    Item *Of = Find(Id);
    if (!Of) return ERR_OBJECT_NOT_FOUND;
    /* A text's bytes, said or in a style, held to the bound before they are
     * taken: the value's length is never less than the text it brings. */
    const std::size_t Before = TextBytes(Of->Said);
    const bool        Texts  = Property == ITEM_PROP_TEXT || Property == ITEM_PROP_CAPTION ||
                               Property == ITEM_PROP_TEXT_STYLE || Property == ITEM_PROP_TEXT_SPANS;
    if (Texts && Held.TextBytes + Value.size() > ITEM_TEXTS_MOST) return ERR_QUOTA_EXCEEDED;
    Uses(*Of, false);
    const error_t Decoded = Decode(Property, Value, Of->Said, Of->Role);
    Uses(*Of, true);
    if (Decoded != OK) return Decoded;
    Held.TextBytes = Held.TextBytes - Before + TextBytes(Of->Said);
    const Asks Wants = AsksOf(Property);
    Mark(*Of, Wants.Picture, Wants.Place);
    return OK;
}

error_t Tree::Reset(std::uint32_t Id, std::uint16_t Property) noexcept
{
    Item *Of = Find(Id);
    if (!Of) return ERR_OBJECT_NOT_FOUND;
    const std::size_t Before = TextBytes(Of->Said);
    Uses(*Of, false);
    const error_t Unsaid = Unsay(Property, Of->Said);
    if (Property == ITEM_PROP_ROLE) Of->Role = ITEM_ROLE_NONE;
    Uses(*Of, true);
    if (Unsaid != OK) return Unsaid;
    Held.TextBytes = Held.TextBytes - Before + TextBytes(Of->Said);
    const Asks Wants = AsksOf(Property);
    Mark(*Of, Wants.Picture, Wants.Place);
    return OK;
}

void Tree::BlockChanged(std::uint32_t Block) noexcept
{
    const auto Found = BlockUsers.find(Block);
    if (Found == BlockUsers.end()) return;
    for (std::uint32_t Id : Found->second)
        if (Item *Of = Find(Id)) Mark(*Of, true, false);
}

void Tree::RedrawAll() noexcept
{
    for (auto &[Id, Each] : All) {
        Each.Redraw = Each.Moved = Each.Within = true;
    }
    AnyMarked = true;
}

}  // namespace items
