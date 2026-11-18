#ifndef BOXOS_DISPLAY_COMPOSITOR_H
#define BOXOS_DISPLAY_COMPOSITOR_H

// The compositor: what the programs' areas show, put together onto the
// screen's canvas only where something changed (the owner's design, part 7).
//
// Each frame it takes the areas' marks: an item whose look changed has its
// drawing made again (item_draw.h), and where every changed item was and now
// is becomes the frame's damage — only that is composed, from the drawings
// kept, over what lies beneath (the console's cells, drawn there first by the
// easel). An item a plain opaque rectangle covers wholly is not drawn at all,
// nor is anything beneath it. A group's items are drawn within it where it
// clips them, and through a picture of their own where the group is not wholly
// opaque, so they show through it together and not through one another.
//
// What it keeps of an item's drawing is bounded per program (KeptMost): past
// it, an item is drawn straight each time it is needed.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "item_draw.h"
#include "item_tree.h"
#include "painter/core/canvas.h"
#include "painter/core/geometry.h"
#include "painter/core/transform.h"

namespace items {

// The bytes of item drawings one program may keep.
constexpr std::size_t KeptMost = std::size_t{192} << 20;

// What one program draws from, over all its areas.
struct ProgramState {
    Budget      Held;
    Sources     From;
    std::size_t KeptBytes = 0;
};

// A step of composing an area: an item drawn, or a group's clip or picture
// begun or ended.
struct Entry {
    enum class Kind : std::uint8_t { Draw, ClipBegin, ClipEnd, LayerBegin, LayerEnd };
    Kind                      What    = Kind::Draw;
    const Item               *Of      = nullptr;
    painter::core::Transform  Place{};
    float                     Opacity = 1.0f;
    painter::core::Rect       Device{};       // what it covers on the glass; a layer's, its items'
    painter::core::Rect       Hides{};        // what it covers whole and opaque; none for most
};

struct AreaState {
    explicit AreaState(ProgramState &Of) : Program(Of), Items(Of.Held) {}

    ProgramState        &Program;
    Tree                 Items;
    painter::core::Rect  Place{};        // on the glass, in pixels
    painter::core::Rect  Was{};          // where it was composed last
    bool                 Shown    = false;
    bool                 WasShown = false;
    bool                 Paged    = false;   // the theme's page beneath it: the whole screen's
    std::vector<Entry>   List;
    // An item sized by its content came to a size: told to the program.
    std::function<void(std::uint32_t, ItemPoint)> Sized;
};

class Compositor {
public:
    // The screen's pixels: damage is kept within them.
    explicit Compositor(painter::core::Rect Screen) : Screen(Screen) { Waiting.reserve(DamageKept); }

    // Pixels to be composed again — something beneath the areas changed.
    void Damage(painter::core::Rect Area) noexcept;
    // Every area's marks taken: drawings made again where marked, and where
    // anything changed added to the damage. Areas bottom to top.
    void Take(std::span<AreaState *const> Areas, const Theme &Look);
    // The damage as rectangles, few and apart, taken.
    std::vector<painter::core::Rect> Rects();
    // Whether an opaque item covers Area whole: what lies beneath it, the
    // console's cells too, need not be drawn.
    bool Hidden(painter::core::Rect Area, std::span<AreaState *const> Areas) const noexcept;
    // Whether any damage waits to be composed.
    bool Damaged() const noexcept { return !Waiting.empty(); }
    // Area composed onto Target, whose corner lies at Corner on the screen,
    // from the areas' items, over what Target holds there; Areas bottom to
    // top.
    void Compose(painter::core::Canvas &Target, painter::core::Point Corner, painter::core::Rect Area,
                 std::span<AreaState *const> Areas, const Theme &Look);

private:
    void Walk(AreaState &Area, const std::vector<std::uint32_t> &Items, const painter::core::Transform &Parent,
              float Opacity, bool Moved, bool Plain);
    void Vanish(AreaState &Area, const std::vector<std::uint32_t> &Items);

    /* The rectangles damage is noted in: past them, joined. */
    static constexpr std::size_t DamageKept = 64;

    painter::core::Rect              Screen;
    std::vector<painter::core::Rect> Waiting;
};

// An item's own placing: its place, then its turn and scale about its anchor.
painter::core::Transform LocalOf(const Item &Of) noexcept;

}  // namespace items

#endif
