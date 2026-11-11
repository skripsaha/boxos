/* look.h — what the person set of the forest (settings forest): read whole,
 * and read again when the file is saved or the Use Context changes. */
#ifndef FOREST_LOOK_H
#define FOREST_LOOK_H

#include <cstddef>
#include <cstdint>

#include "box/types.h"
#include "key_chord.h"
#include "kinds.h"
#include "numbers.h"

namespace forest {

// The keys a person chooses, in the order of Look::Keys; the rest are the
// arrows and Esc, which no setting takes.
enum ForestKey : std::uint8_t { KeySeason, KeyWeather, KeyTime, KeyPause, KeyHelp, KeyCount };

struct Look {
    bool          SeasonByDate;     // the season is the date's, not the one chosen
    Season        StartSeason;
    Weather       StartWeather;
    bool          RealTime;         // the day is the machine's clock, not a chosen one
    std::uint32_t StartMinute;      // the minute of the day a chosen time starts at
    std::uint32_t Speed;            // times real time, when the time is chosen
    bool          Drift;            // the weather changes by itself
    std::uint32_t Animals[KindCount];
    Color         Sky;              // a clear noon's
    Color         Leaves;           // summer's
    Color         Autumn;           // autumn's brightest
    Color         Blossom;
    Color         Bark;
    Color         Grass;
    Color         Snow;
    Color         BarPaper;         // the line at the bottom
    Color         BarInk;
    Color         BarKey;
    KeyChord      Keys[KeyCount];
};

Look ReadLook();

}  // namespace forest

#endif
