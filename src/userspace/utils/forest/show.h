/* show.h — the forest on the glass: the clock it keeps, the keys it hears and
 * the one line it says them on. */
#ifndef FOREST_SHOW_H
#define FOREST_SHOW_H

#include <cstdint>
#include <optional>

#include "box/cxx/settings.h"
#include "forest.h"
#include "painter/tui/stage.h"

namespace forest {

// A time of day the person named: the sun's own, whatever the season.
enum class Part : std::uint8_t { Dawn, Noon, Dusk, Night };

// What the person said on the line, and so what the settings do not decide.
struct Wanted {
    std::optional<Season>  Of;
    std::optional<Weather> Is;
    std::optional<int>     Minute;      // a time of day: it is chosen then, and flows
    std::optional<Part>    At;
};

// The settings with the line's words over them.
Look Said(const Look &Set, const Wanted &Words, const Date &Day);

// The machine's day, and the minute of it with the seconds in its fraction.
void ReadMachineMoment(Date &Day, float &Minute);

// What the line at the bottom says.
struct Saying {
    Season        Of;
    Weather       Is;
    int           Hour, Minute;
    bool          Real;                 // the machine's clock, not a chosen time
    bool          Paused;
    std::uint32_t Speed;
    std::size_t   Living;
};

void PaintBar(painter::tui::Frame &Glass, const Look &Chosen, const Saying &Say);

class Show {
public:
    // `Set` is what the settings say, `Starting` what the forest begins as:
    // the same, but for the words on the line.
    Show(painter::tui::Stage Opened, const Look &Set, const Look &Starting, const Date &Day, float Minute);

    // False when the screen stopped taking frames.
    bool Run();
    void Close() { (void)Screen.Close(); }

    std::uint32_t Frames = 0;
    std::size_t   Living() const { return Forest.Herd.Living(); }
    Season        CurrentSeason() const { return Forest.Weather.CurrentSeason(); }
    Weather       CurrentWeather() const { return Forest.Weather.CurrentWeather(); }

private:
    bool Draw();
    bool OnKey(const painter::tui::KeyEvent &Key);
    void Tick(float Seconds);
    void Resync();
    void Reread();
    void Say(float Seconds);

    painter::tui::Stage  Screen;
    int                  Cols, Rows;
    Look                 Settled;        // as the settings last said
    Look                 Chosen;
    Canvas               Picture;
    World                Forest;
    box::settings::watch Watch;
    bool                 Real;
    bool                 Paused = false;
    std::uint32_t        Speed;
    bool                 Help   = false; // the line stays, and does not go after a few seconds
    float                SayUntil = 8.0f;
    float                SinceClock = 0;
};

}  // namespace forest

#endif
