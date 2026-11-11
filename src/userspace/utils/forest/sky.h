/* sky.h — what the sky is like now: where the sun and the moon stand, how
 * light the day is, the colours of the sky from its top to its horizon, and the
 * light that falls on everything under it. Worked out from the time of day, the
 * day of the year and the weather — the sun's day is longer in summer and
 * stands higher, the moon is the real moon's phase for the date. */
#ifndef FOREST_SKY_H
#define FOREST_SKY_H

#include "look.h"
#include "nature.h"
#include "numbers.h"

namespace forest {

struct SkyNow {
    float Day       = 1;       // 0 night .. 1 day, through the twilights
    float Glow      = 0;       // 0..1 how much the horizon burns
    bool  SunUp     = true;
    float SunX      = 0.5f;    // 0..1 across the sky
    float SunHeight = 0.5f;    // 0 the horizon .. 1 the top of the sky
    bool  MoonUp    = false;
    float MoonX     = 0.5f;
    float MoonHeight = 0.5f;
    float MoonRound = 0;       // 0 new, 0.5 full, 1 new again
    float MoonLight = 0;       // 0..1 how much of its face is lit
    Color Top;
    Color Horizon;             // also the colour far things fade to
    Tint  Light;               // on what stands in the day
};

SkyNow ReadSky(const Nature &Weather, const Look &Shown);

// When the sun rises and sets on a day of the year, in minutes of the day.
float SunriseMinute(int DayOfYear);
float SunsetMinute(int DayOfYear);

}  // namespace forest

#endif
