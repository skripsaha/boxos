/* forest — a living forest in half-block pixels: oaks, birches and spruces
 * three trees deep, a meadow, and the day over them — the sun and the real
 * moon, the weather, the seasons. Foxes trot and sniff and pounce in the snow,
 * hedgehogs shuffle and curl up and sleep the winter, hares sit and hop and
 * bolt, deer graze at dawn and dusk, birds fly, perch and sing, owls keep the
 * night, and bats, butterflies and fireflies drift in their hours.
 *
 *   forest                  the forest as the machine's clock and date say
 *   forest autumn           begin in a season: spring, summer, autumn, winter
 *   forest storm            begin in a weather: clear, rain, snow, fog, wind, storm
 *   forest dusk             begin at a time: dawn, noon, dusk, night, or 18:30
 *
 *   left, right             turn the time back or on, half an hour
 *   up, down                the day goes faster or slower
 *   ctrl+s  ctrl+w          the next season, the next weather
 *   ctrl+t  ctrl+p          the machine's clock or a chosen time; stop the day
 *   ctrl+?                  the line of keys, shown and hidden
 *   esc, q                  leave, and say how it went
 *
 * settings forest sets who lives there and how many, how it begins, how fast a
 * chosen day goes, the colours and the keys; they take effect while it runs.
 * What it is made of: look (the settings), nature and sky (the day, the season,
 * the weather), scenery (what stands still), air (what falls), fauna and
 * sprites (who lives there), paint (the picture) and show (the clock, the keys,
 * the loop). Only this file, show, bar and look know BoxOS.
 */

#include <charconv>
#include <cstring>
#include <format>
#include <new>
#include <optional>
#include <string>
#include <string_view>

#include "box/form.h"
#include "box/print.h"

#include "show.h"

BOX_FORM("takes optional words \"a season (spring, summer, autumn, winter), a weather (clear, rain, snow, fog, "
         "wind, storm) and a time (dawn, noon, dusk, night or like 18:30) to begin with\"\n");

namespace {

using namespace forest;

constexpr std::int32_t LeastCols = 60;
constexpr std::int32_t LeastRows = 20;

void Say(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_to_screen(Line.c_str());
}

struct SeasonWord {
    const char *Word;
    Season      Is;
};

struct WeatherWord {
    const char *Word;
    Weather     Is;
};

struct PartWord {
    const char *Word;
    Part        Is;
};

const SeasonWord Seasons[] = {{"spring", Season::Spring}, {"summer", Season::Summer}, {"autumn", Season::Autumn}, {"winter", Season::Winter}};
const WeatherWord Weathers[] = {{"clear", Weather::Clear}, {"rain", Weather::Rain}, {"snow", Weather::Snow},
                             {"fog", Weather::Fog},     {"wind", Weather::Wind}, {"storm", Weather::Storm}};
const PartWord Parts[] = {{"dawn", Part::Dawn}, {"noon", Part::Noon}, {"dusk", Part::Dusk}, {"night", Part::Night}};

// "18:30" or "6:05" as minutes of the day.
std::optional<int> ClockWord(std::string_view Word)
{
    std::size_t Colon = Word.find(':');
    if (Colon == std::string_view::npos) return std::nullopt;
    int Hours = 0, Minutes = 0;
    auto [HoursEnd, HoursFault] = std::from_chars(Word.data(), Word.data() + Colon, Hours);
    auto [MinutesEnd, MinutesFault] = std::from_chars(Word.data() + Colon + 1, Word.data() + Word.size(), Minutes);
    if (HoursFault != std::errc() || HoursEnd != Word.data() + Colon || MinutesFault != std::errc() ||
        MinutesEnd != Word.data() + Word.size() || Hours > 23 || Minutes > 59)
        return std::nullopt;
    return Hours * 60 + Minutes;
}

// One word of the line into what it names; false once it has said why not.
bool TakeWord(std::string_view Word, Wanted &Words)
{
    for (const SeasonWord &Each : Seasons)
        if (Word == Each.Word) {
            if (Words.Of) {
                Say(std::format("forest: two seasons, {} and {}: one will do.", SeasonName(*Words.Of), Word));
                return false;
            }
            Words.Of = Each.Is;
            return true;
        }
    for (const WeatherWord &Each : Weathers)
        if (Word == Each.Word) {
            if (Words.Is) {
                Say(std::format("forest: two weathers, {} and {}: one will do.", WeatherName(*Words.Is), Word));
                return false;
            }
            Words.Is = Each.Is;
            return true;
        }
    std::optional<Part> Named;
    for (const PartWord &Each : Parts)
        if (Word == Each.Word) Named = Each.Is;
    std::optional<int> Clock = ClockWord(Word);
    if (!Named && !Clock) {
        Say(std::format("forest: what is {}? It takes a season, a weather and a time: spring, rain, dusk, 18:30.", Word));
        return false;
    }
    if (Words.At || Words.Minute) {
        Say("forest: two times: one will do.");
        return false;
    }
    Words.At     = Named;
    Words.Minute = Clock;
    return true;
}

bool ReadWords(const char *const *Given, std::size_t Count, Wanted &Words)
{
    for (std::size_t At = 0; At < Count; At++)
        if (!TakeWord(Given[At], Words)) return false;
    return true;
}

}  // namespace

int main()
{
    try {
        const char *const *Given = nullptr;
        std::size_t        Count = 0;
        if (form_words(0, &Given, &Count) != 0) return 1;
        Wanted Words;
        if (!ReadWords(Given, Count, Words)) return 1;

        Date  Day;
        float Minute = 720.0f;
        ReadMachineMoment(Day, Minute);
        const Look Set      = ReadLook();
        const Look Starting = Said(Set, Words, Day);
        if (Words.Is == Weather::Snow && (Starting.SeasonByDate ? SeasonOfDate(Day) : Starting.StartSeason) != Season::Winter) {
            Say("forest: snow falls in winter only; say winter too.");
            return 1;
        }
        if (!Starting.RealTime) Minute = static_cast<float>(Starting.StartMinute);

        std::expected<painter::tui::Stage, painter::tui::StageError> Opened = painter::tui::Stage::Open(LeastCols, LeastRows);
        if (!Opened) {
            Say(Opened.error().Describe("forest"));
            return 1;
        }
        MakeWaves();
        Show Forest(std::move(*Opened), Set, Starting, Day, Minute);
        bool Painted = Forest.Run();
        Forest.Close();
        std::size_t Living = Forest.Living();
        Say(std::format("[FOREST] {} frames - {}, {}, {} {} about", Forest.Frames, SeasonName(Forest.CurrentSeason()),
                        WeatherName(Forest.CurrentWeather()), Living, Living == 1 ? "animal" : "animals"));
        if (!Painted) Say("The screen stopped taking frames.");
        return Painted ? 0 : 1;
    } catch (const std::bad_alloc &) {
        Say("forest: there is not enough memory for a forest the size of this screen.");
        return 1;
    }
}
