/* world.cpp — the forest made, and a step of its life. */

#include "forest.h"

namespace forest {

World::World(const Look &Wanted, int Width, int Height, std::uint64_t Seed, const Date &Day, float Minute)
    : Chosen(Wanted), Chance(Seed), Weather(Wanted.StartSeason, Wanted.SeasonByDate, Wanted.StartWeather),
      Place(MakeScenery(Width, Height, Chance))
{
    Weather.SetMoment(Day, Minute);
    Weather.Drifts(Wanted.Drift);
    Weather.Arrive();
    Sky = ReadSky(Weather, Chosen);
    Herd.Fit(Chosen, Here(), true);
}

void World::Step(float Seconds)
{
    Weather.Step(Seconds, Chance);
    Sky = ReadSky(Weather, Chosen);
    Air.Step(Weather, Place, Chance, Seconds);
    Herd.Step(Here(), Seconds);
}

void World::Apply(const Look &Now)
{
    Chosen = Now;
    Weather.Drifts(Now.Drift);
    Herd.Fit(Chosen, Here(), false);
}

}  // namespace forest
