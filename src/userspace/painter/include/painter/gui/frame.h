#ifndef PAINTER_GUI_FRAME_H
#define PAINTER_GUI_FRAME_H

// A frame described whole (Area::Draw): its items made here as they are made
// in an area — Text, Rectangle, Group… — and told what they show this time.
//
//     Screen->Draw([&](gui::Frame &F) {
//         F.Rectangle(gui::Role::Panel).SetSize({200, 60});
//         for (const Row &Each : Rows) F.Text(Each.Name).Key(Each.Id).SetPlace(Each.At);
//     });
//
// Once it is described, each item is matched to one of the frame before's —
// under the same parent, of the same kind, at the same place among its
// siblings, or saying the same Key — and only what differs is sent, as one
// step: what is new made, what changed said, what it no longer says unsaid,
// what is gone removed, what moved among its siblings restacked. Items lie in
// the order they were described, the first lowest. Its objects say what the
// frame is and nothing after: the items are the frame's.

#include <compare>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "painter/gui/changes.h"
#include "painter/gui/items.h"

namespace painter::gui {

namespace detail {
class Drawer;
}

class Frame : public Makes<Frame> {
public:
    Frame(const Frame &)            = delete;
    Frame &operator=(const Frame &) = delete;
    ~Frame();

private:
    // An item as it is described: its kind and role, its parent (a number of
    // this frame, ITEM_AREA for the area) and its key — its place among its
    // siblings, or what it says.
    struct Described {
        gui::Kind     Kind   = gui::Kind::Group;
        gui::Role     Role   = gui::Role::None;
        std::uint32_t Parent = ITEM_AREA;
        std::uint64_t Key    = 0;
        bool          Keyed  = false;
    };

    Frame();
    const std::shared_ptr<detail::Anchor> &Where() const noexcept { return Anchored; }
    std::uint32_t                          Parent() const noexcept { return ITEM_AREA; }
    // An item described under Parent: its number in this frame, from 1.
    std::uint32_t Describe(gui::Kind Kind, gui::Role Role, std::uint32_t Parent);
    void          Keyed(std::uint32_t Number, std::uint64_t Key) noexcept;

    Changes                                 Said;      // what its items say, by their numbers here
    std::vector<Described>                  List;      // in the order described: number N is List[N - 1]
    std::map<std::uint32_t, std::uint64_t>  Placed;    // how many are described under each parent
    std::shared_ptr<detail::Anchor>         Anchored;

    friend class Item;
    friend class detail::Drawer;
    template <typename Self>
    friend class Makes;
    friend detail::Made detail::Make(const std::shared_ptr<detail::Anchor> &, std::uint32_t, gui::Kind, gui::Role);
};

namespace detail {

// Frames drawn into the changes an anchor names, numbered from its numbers:
// the frame drawn last kept — each item with what it is found again by — the
// next one described, and only what differs said. Area::Draw's way, and
// painter's own stand's.
class Drawer {
public:
    explicit Drawer(std::shared_ptr<Anchor> Where) : Where(std::move(Where)) {}
    Frame &Begin();
    // The frame described, matched to the one before and what differs said;
    // nothing when the anchor's changes are gone.
    void End();

private:
    // An item as the next frame finds it again: under the same parent, of
    // the same kind, at the same place among its siblings or saying the same
    // key.
    struct Match {
        std::uint32_t Parent = ITEM_AREA;
        std::uint8_t  Kind   = 0;
        bool          Keyed  = false;
        std::uint64_t Key    = 0;
        auto operator<=>(const Match &) const = default;
    };
    struct Was {
        Match         By;
        std::uint32_t Number = 0;
        gui::Role     Role   = gui::Role::None;
        bool          Again  = false;   // of the frame drawn: an item of the frame before, found again
        bool          Found  = false;   // of the frame before: found again in the frame drawn
    };
    using Placed = std::pair<std::uint32_t, std::uint32_t>;   // a parent's number, an item's place
    std::shared_ptr<Anchor>                 Where;
    std::vector<Was>                        Items;        // the frame drawn last, in the order described
    std::unique_ptr<Frame>                  Describing;
    /* What End matches with, kept from frame to frame so that a frame makes
     * none of it anew: the frame before's items by what they are found by,
     * the frame drawn, its numbers and the last item laid under each of its
     * items, the items under each parent, the properties unsaid. */
    std::vector<std::pair<Match, std::uint32_t>> Last;
    std::vector<Was>                             Drawn;
    std::vector<std::uint32_t>                   Numbered, Under;
    std::vector<Placed>                          Laid, Before;
    std::vector<std::uint16_t>                   Unsaid;
};

}  // namespace detail

}  // namespace painter::gui

#endif  // PAINTER_GUI_FRAME_H
