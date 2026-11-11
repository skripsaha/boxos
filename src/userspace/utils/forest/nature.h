/* nature.h — the day, the season and the weather, and how the forest comes to
 * each: the leaves, the snow and the blossom on the land, the rain, the fog,
 * the wind and the cloud in the air, all easing toward what the season and the
 * weather ask for, so a change is a few seconds of weather, never a cut. */
#ifndef FOREST_NATURE_H
#define FOREST_NATURE_H

#include "kinds.h"
#include "numbers.h"

namespace forest {

struct Date {
    int Year  = 2026;
    int Month = 6;
    int Day   = 21;
};

// What the season has done to the land, each 0..1.
struct Land {
    float Leaves  = 0;    // how much of the canopy is there
    float Autumn  = 0;    // how far the leaves have turned
    float Blossom = 0;
    float Snow    = 0;    // cover on the ground and the boughs
    float Flowers = 0;
    float Litter  = 0;    // fallen leaves, and the mushrooms among them
};

// What the weather has done to the air, each 0..1.
struct Air {
    float Rain   = 0;
    float Flakes = 0;     // snow falling
    float Fog    = 0;
    float Wind   = 0;
    float Cloud  = 0;
    float Gloom  = 0;     // how dark the sky has gone
    float Storm  = 0;     // lightning
};

class Nature {
public:
    Nature(Season Begin, bool ByDate, Weather Start);

    // `Seconds` of weather: the land and the air ease, the weather drifts if
    // it may, and the animation clock goes on.
    void Step(float Seconds, Dice &Chance);

    // The land and the air at once as the season and the weather ask: how the
    // forest is when it opens.
    void Arrive();

    void SetSeason(Season Value);
    void FollowDate();
    void SetWeather(Weather Value);
    void NextSeason();
    void NextWeather();
    void Drifts(bool Value) { Drifting = Value; }

    // The machine's clock: what day and what minute it is.
    void SetMoment(const Date &Day, float Minute);
    // The day goes on by `Minutes`, over midnight into the next.
    void Advance(float Minutes);

    Season  CurrentSeason() const { return SeasonNow; }
    Weather CurrentWeather() const { return WeatherNow; }

    // The day of the year the sun is read for, 0..364, and how far the moon
    // has gone round, 0..1 from new to new.
    int   DayOfYear() const;
    float MoonRound() const;

    Land  Ground;
    Air   Aloft;
    float Minutes = 480;       // of the day
    float Clock   = 0;         // seconds of animation, for every wave

private:
    void  KeepWeatherFit();
    void  PickWeather(Dice &Chance);
    void  Shift(int Days);

    Season  SeasonNow;
    Weather WeatherNow;
    Date    Today;
    bool    Following;
    bool    Drifting  = false;
    float   DriftLeft = 90;
};

// Which season a date is in, by the months: March to May is spring.
Season SeasonOfDate(const Date &Day);

// Where in the year a date is, 0..364, and the middle of a season's.
int DayInYear(const Date &Day);
int SeasonMiddle(Season Value);

}  // namespace forest

#endif
