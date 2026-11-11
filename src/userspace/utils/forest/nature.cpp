/* nature.cpp — the seasons and the weathers, and the days between. */

#include "nature.h"

#include <cmath>

namespace forest {

namespace {

constexpr float LandRate = 0.1f;     // a season's change takes some twenty seconds
constexpr float AirRate  = 0.35f;    // a weather's, a few
constexpr float SynodicDays = 29.530588f;

// A new moon: 2000-01-06 at 18:14, as days after 1970-01-01.
constexpr float NewMoonDay = 10962.76f;

Land LandFor(Season Value)
{
    switch (Value) {
    case Season::Spring: return {0.85f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f};
    case Season::Summer: return {1.0f, 0.0f, 0.0f, 0.0f, 0.8f, 0.0f};
    case Season::Autumn: return {0.7f, 1.0f, 0.0f, 0.0f, 0.1f, 1.0f};
    case Season::Winter: return {0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    }
    return {};
}

Air AirFor(Weather Value)
{
    switch (Value) {
    case Weather::Clear: return {0.0f, 0.0f, 0.0f, 0.15f, 0.25f, 0.0f, 0.0f};
    case Weather::Rain:  return {1.0f, 0.0f, 0.1f, 0.3f, 0.85f, 0.55f, 0.0f};
    case Weather::Snow:  return {0.0f, 1.0f, 0.25f, 0.15f, 0.8f, 0.4f, 0.0f};
    case Weather::Fog:   return {0.0f, 0.0f, 1.0f, 0.05f, 0.5f, 0.25f, 0.0f};
    case Weather::Wind:  return {0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 0.1f, 0.0f};
    case Weather::Storm: return {1.0f, 0.0f, 0.1f, 0.9f, 1.0f, 0.9f, 1.0f};
    }
    return {};
}

int DaysIn(int Year, int Month)
{
    static constexpr int Days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool Leap = (Year % 4 == 0 && Year % 100 != 0) || Year % 400 == 0;
    return Days[Month - 1] + (Month == 2 && Leap ? 1 : 0);
}

// Days after 1970-01-01 (the civil calendar, proleptic Gregorian).
int DaysSinceEpoch(const Date &Day)
{
    int Year    = Day.Year - (Day.Month <= 2 ? 1 : 0);
    int Era     = (Year >= 0 ? Year : Year - 399) / 400;
    int InEra   = Year - Era * 400;
    int InYear  = (153 * (Day.Month + (Day.Month > 2 ? -3 : 9)) + 2) / 5 + Day.Day - 1;
    int InCycle = InEra * 365 + InEra / 4 - InEra / 100 + InYear;
    return Era * 146097 + InCycle - 719468;
}

Land Eased(const Land &From, const Land &To, float Seconds)
{
    return {Ease(From.Leaves, To.Leaves, LandRate, Seconds),   Ease(From.Autumn, To.Autumn, LandRate, Seconds),
            Ease(From.Blossom, To.Blossom, LandRate, Seconds), Ease(From.Snow, To.Snow, LandRate, Seconds),
            Ease(From.Flowers, To.Flowers, LandRate, Seconds), Ease(From.Litter, To.Litter, LandRate, Seconds)};
}

Air Eased(const Air &From, const Air &To, float Seconds)
{
    return {Ease(From.Rain, To.Rain, AirRate, Seconds),   Ease(From.Flakes, To.Flakes, AirRate, Seconds),
            Ease(From.Fog, To.Fog, AirRate, Seconds),     Ease(From.Wind, To.Wind, AirRate, Seconds),
            Ease(From.Cloud, To.Cloud, AirRate, Seconds), Ease(From.Gloom, To.Gloom, AirRate, Seconds),
            Ease(From.Storm, To.Storm, AirRate, Seconds)};
}

}  // namespace

Season SeasonOfDate(const Date &Day)
{
    if (Day.Month >= 3 && Day.Month <= 5) return Season::Spring;
    if (Day.Month >= 6 && Day.Month <= 8) return Season::Summer;
    if (Day.Month >= 9 && Day.Month <= 11) return Season::Autumn;
    return Season::Winter;
}

int DayInYear(const Date &Day)
{
    int Count = Day.Day - 1;
    for (int Month = 1; Month < Day.Month; Month++) Count += DaysIn(Day.Year, Month);
    return Count;
}

// The middle of each season, for the sun when the season is the person's.
int SeasonMiddle(Season Value)
{
    switch (Value) {
    case Season::Spring: return 105;
    case Season::Summer: return 196;
    case Season::Autumn: return 288;
    case Season::Winter: return 15;
    }
    return 196;
}

Nature::Nature(Season Begin, bool ByDate, Weather Start)
    : SeasonNow(Begin), WeatherNow(Start), Following(ByDate)
{
    KeepWeatherFit();
}

void Nature::Arrive()
{
    Ground = LandFor(SeasonNow);
    Aloft  = AirFor(WeatherNow);
}

void Nature::Step(float Seconds, Dice &Chance)
{
    Clock += Seconds;
    Ground = Eased(Ground, LandFor(SeasonNow), Seconds);
    Aloft  = Eased(Aloft, AirFor(WeatherNow), Seconds);
    if (!Drifting) return;
    DriftLeft -= Seconds;
    if (DriftLeft <= 0) PickWeather(Chance);
}

// Snow falls in winter only: out of it, what was snow is rain.
void Nature::KeepWeatherFit()
{
    if (WeatherNow == Weather::Snow && SeasonNow != Season::Winter) WeatherNow = Weather::Rain;
}

void Nature::PickWeather(Dice &Chance)
{
    struct Odds {
        Weather Which;
        float   Weight;
    };
    const Odds Table[] = {{Weather::Clear, 45}, {Weather::Rain, 18}, {Weather::Fog, 10}, {Weather::Wind, 12},
                          {Weather::Storm, 5},  {Weather::Snow, SeasonNow == Season::Winter ? 24.0f : 0.0f}};
    float Total = 0;
    for (const Odds &Each : Table) Total += Each.Weight;
    float Roll = Chance.Between(0, Total);
    for (const Odds &Each : Table) {
        Roll -= Each.Weight;
        if (Roll <= 0) {
            WeatherNow = Each.Which;
            break;
        }
    }
    DriftLeft = Chance.Between(60.0f, 180.0f);
}

void Nature::SetSeason(Season Value)
{
    SeasonNow = Value;
    Following = false;
    KeepWeatherFit();
}

void Nature::FollowDate()
{
    Following = true;
    SeasonNow = SeasonOfDate(Today);
    KeepWeatherFit();
}

void Nature::SetWeather(Weather Value)
{
    WeatherNow = Value;
    KeepWeatherFit();
}

void Nature::NextSeason()
{
    SetSeason(static_cast<Season>((static_cast<int>(SeasonNow) + 1) % SeasonCount));
}

void Nature::NextWeather()
{
    int Next = (static_cast<int>(WeatherNow) + 1) % WeatherCount;
    if (static_cast<Weather>(Next) == Weather::Snow && SeasonNow != Season::Winter) Next = (Next + 1) % WeatherCount;
    WeatherNow = static_cast<Weather>(Next);
}

void Nature::SetMoment(const Date &Day, float Minute)
{
    Today   = Day;
    Minutes = Minute;
    if (Following) {
        SeasonNow = SeasonOfDate(Today);
        KeepWeatherFit();
    }
}

void Nature::Shift(int Days)
{
    for (; Days > 0; Days--) {
        if (++Today.Day > DaysIn(Today.Year, Today.Month)) {
            Today.Day = 1;
            if (++Today.Month > 12) {
                Today.Month = 1;
                Today.Year++;
            }
        }
    }
    for (; Days < 0; Days++) {
        if (--Today.Day < 1) {
            if (--Today.Month < 1) {
                Today.Month = 12;
                Today.Year--;
            }
            Today.Day = DaysIn(Today.Year, Today.Month);
        }
    }
    if (Following) {
        SeasonNow = SeasonOfDate(Today);
        KeepWeatherFit();
    }
}

void Nature::Advance(float Added)
{
    Minutes += Added;
    while (Minutes >= 1440.0f) {
        Minutes -= 1440.0f;
        Shift(1);
    }
    while (Minutes < 0.0f) {
        Minutes += 1440.0f;
        Shift(-1);
    }
}

int Nature::DayOfYear() const
{
    return Following ? DayInYear(Today) : SeasonMiddle(SeasonNow);
}

float Nature::MoonRound() const
{
    float Days = static_cast<float>(DaysSinceEpoch(Today)) + Minutes / 1440.0f - NewMoonDay;
    float Round = std::fmod(Days, SynodicDays) / SynodicDays;
    return Round < 0 ? Round + 1.0f : Round;
}

}  // namespace forest
