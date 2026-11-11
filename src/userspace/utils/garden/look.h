/* look.h — what the person set of the garden (settings garden): read whole,
 * and read again when the file is saved or the Use Context changes. */
#ifndef GARDEN_LOOK_H
#define GARDEN_LOOK_H

#include <cstdint>

#include "box/types.h"
#include "key_chord.h"
#include "painter/core/color.h"

namespace garden {

using Color = painter::core::Color;

// What ranks the plants.
enum class Order { Starts, Recent, Name };
// The hour of the sky and the season of the plants: the machine's, or fixed.
enum class Hour { Clock, Day, Dusk, Night };
enum class Season { Clock, Spring, Summer, Autumn, Winter };

// The keys a person chooses, in the order of Look::Keys; the rest are the
// arrows, Space and Esc, which no setting takes.
enum GardenKey : std::uint8_t { KeyView, KeyOrder, KeyHelp, KeyCount };

struct Look {
    std::uint32_t Shown;           // the most plants shown
    std::uint32_t WiltDays;        // unused for so many days, a plant is wilted
    std::uint32_t Speed;           // wind and wings, percent
    Order         Ranking;
    bool          System;          // the machine's own programs grow here too
    bool          Itself;          // and garden's own plant
    Hour          Sky;
    Season        Year;
    bool          Creatures;
    Color         Day;             // the sky by day,
    Color         Dusk;            //   at dusk and dawn,
    Color         Night;           //   by night
    Color         Grass;
    Color         Soil;
    Color         Leaf;
    Color         Wilted;
    Color         BloomA;          // the flowers, one of three to a plant
    Color         BloomB;
    Color         BloomC;
    Color         Fruit;
    Color         Bars;            // the lines at the top and the bottom
    Color         Title;
    Color         Text;
    Color         Quiet;
    Color         Chosen;          // behind the chosen plant's label
    Color         Marked;          // a marked plant's mark
    KeyChord      Keys[KeyCount];
};

Look ReadLook();

}  // namespace garden

#endif
