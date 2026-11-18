#ifndef BOXOS_DISPLAY_ANIMATOR_H
#define BOXOS_DISPLAY_ANIMATOR_H

// An area's animations (box/items.h ITEM_OP_ANIMATE), played by the clock
// while the program sleeps.
//
// An animation said waits for its step to be whole; then it begins after its
// delay, or once the animations of the number it waits on have all ended. As
// it begins it takes its property from where the item shows it — cutting the
// one under way there, whose speed a spring takes on — and goes:
//   - through the numbers between, where the property has them and both ends
//     are of one form (painter/items/between.h): each frame the value on its
//     way is said into the item, which is drawn again only as that property
//     asks — moved, or its picture made again;
//   - fading from the look it had into the new one, where the property has
//     no numbers between — a text's characters, a picture, a role, a look of
//     another form: the item's last drawing kept as it was and laid with the
//     new one, the two weighed by how far it has faded;
//   - in or out of sight, for hidden: how much of it is seen walking to whole
//     or to none, and hidden said only when none is;
//   - at once, for what has nothing to show on the way.
// At its end the property is exactly what the program said, said as a plain
// change says it. A property said plainly or unsaid, or its item gone, cuts
// its animations; the end of a number's last animation is told back.
//
// Time is the clock's: where an animation stands is reckoned from when it
// began, not counted in frames, so a late frame shows it where it is and a
// chain's next begins at the very moment the one before ended. A frame is
// wanted only while something seen moves; otherwise only the moments
// something begins or ends.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <vector>

#include "box/error.h"
#include "box/items.h"
#include "item_tree.h"
#include "painter/core/easing.h"
#include "painter/items/between.h"

namespace items {

// How the person wants things to move, for every area alike.
struct Motion {
    bool          Less    = false;   // things moved, sized, turned or reshaped at once; fades short
    std::uint32_t Speed   = 100;     // percent of the time each takes
    bool          Instant = false;   // a text page: every change at once
};

// A frame while something seen moves: sixty a second.
constexpr std::uint64_t FrameMicros = 16667;

class Animator {
public:
    using Ending = std::function<void(std::uint32_t Item, const ItemAnimationEnd &End)>;

    Animator(Tree &Items, Budget &Held, const Motion &Wanted) noexcept : Items(Items), Held(Held), Wanted(Wanted) {}
    Animator(const Animator &)            = delete;
    Animator &operator=(const Animator &) = delete;
    ~Animator();

    // The moment the steps being read are read at: when what they say begins.
    void Reading(std::uint64_t Now) noexcept { ReadAt = Now; }
    // An animation said: Property of Item to Value, as How says. OK, or why
    // not — the item, the value or how it goes breaks the language or a bound.
    error_t Said(std::uint32_t Item, const ItemAnimation &How, std::uint16_t Property,
                 std::span<const std::uint8_t> Value) noexcept;
    // Property of Item said plainly or unsaid — or, as 0, the item gone: its
    // animations cut where they stand.
    void Cut(std::uint32_t Item, std::uint16_t Property) noexcept;
    // The step whole: each animation said in it waits on its number, or is
    // put to begin after its delay.
    void StepWhole() noexcept;
    // Every animation brought to Now: those due begun and ended in the order
    // of their moments, the rest said where they stand; each number whose
    // last animation ended told through Ended.
    void Advance(std::uint64_t Now, const Theme &Look, const Ending &Ended) noexcept;
    // When the area is next wanted, of the clock's microseconds — the next
    // frame while something moves and the area is Seen, else the next moment
    // something begins or ends; 0 for never.
    std::uint64_t Due(bool Seen) const noexcept;
    std::size_t   Count() const noexcept { return All.size(); }

private:
    enum class State : std::uint8_t { Said, Waits, Due, Under };
    enum class Way : std::uint8_t { AtOnce, Walks, Fades, Shows, Hides };

    struct Animation {
        std::uint32_t             Item     = 0;
        std::uint16_t             Property = 0;
        ItemAnimation             How{};
        std::vector<std::uint8_t> Value;                  // what it ends at, as a plain change says it
        Properties                To;                     // the same, read
        std::uint64_t             Serial   = 0;           // its own, among the area's
        State                     Stage    = State::Said;
        std::uint64_t             Begins   = 0;           // microseconds of the clock
        std::uint64_t             Run      = 0;           // a run's, as it began
        std::uint64_t             Runs     = 1;           // 0 for forever
        Way                       Goes     = Way::AtOnce;
        painter::core::Easing     Curve;
        Numbers                   From, Toward;           // Walks: its numbers at its beginning and end
        std::vector<double>       Speed;                  // Walks: each number's, a second, as it began
        Properties                Like;                   // Walks: the form its numbers are said in
        float                     SeenFrom = 1;           // Shows, Hides: how much was seen as it began
        bool                      Fresh    = false;       // of an item never drawn, a property it never said: at once
    };
    // The animations of a number, under way or waiting, and whether one of
    // them was cut.
    struct Numbered {
        std::uint32_t Count = 0;
        bool          Cut   = false;
    };

    std::uint64_t DelayOf(const Animation &Of) const noexcept;
    bool          WaitsOnItself(std::uint32_t Number, std::uint32_t After) const;
    std::size_t   IndexOf(std::uint64_t Serial) const noexcept;
    void          Begin(std::size_t Index, std::uint64_t At, const Theme &Look);
    void          Show(Animation &Of, std::uint64_t At, const Theme &Look);
    std::vector<double> SpeedOf(const Animation &Of, std::uint64_t At) const;
    void          Finish(std::size_t Index, std::uint64_t At);
    void          End(std::size_t Index, std::uint64_t At, bool Finished) noexcept;
    float         Progress(const Animation &Of, std::uint64_t At, std::uint64_t &RunOf) const noexcept;
    void          Turns(std::uint32_t Item) noexcept;

    Tree                                   &Items;
    Budget                                 &Held;
    const Motion                           &Wanted;
    std::vector<Animation>                  All;          // in the order said
    std::map<std::uint32_t, Numbered>       ByNumber;
    std::vector<std::pair<std::uint32_t, ItemAnimationEnd>> Told;    // to be told: the item, the end
    std::uint64_t                           Serials = 0;
    std::uint64_t                           ReadAt = 0;
    std::uint64_t                           Shown  = 0;   // the last moment brought to
};

}  // namespace items

#endif
