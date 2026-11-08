/* flame — fire on the console, burning from where it is told to.
 *
 *   flame                  fire along the bottom
 *   flame frame gas        fire from all four edges, blue
 *   flame pit #20c0a0      a bonfire in the middle of the bottom
 *   flame BoxOS            the word BoxOS, standing in flames
 *   flame top Hi!          the word wins: a word to burn means `where` is word
 *
 *   ESC                    the menu opens and closes; the fire keeps burning
 *   1-6, p, s, -/+, ,/.    where, colour, sparks, height, wind without the menu
 *   ^Q, q                  leave, and say how many frames it drew
 *
 * The heat is flame_sim.cpp, its colours flame_palette.cpp, the word's pixels
 * flame_font.cpp; the glass is flame_paint.cpp and the menu flame_menu.cpp.
 * Painter owns the screen frame and presents it once per animation step.
 */

#include <algorithm>
#include <format>
#include <new>
#include <string>
#include <string_view>

#include "box/color.h"
#include "box/cpu.h"
#include "box/clock.h"
#include "box/error.h"
#include "box/form.h"
#include "box/print.h"

#include "flame.h"

BOX_FORM("takes optional words \"where it burns and its colour: bottom, top, sides, frame, pit; "
         "fire, gas, toxic, spirit or #rrggbb; any other word burns, and only that word\"\n");

namespace {

/* Frames come by the clock, so a slower machine draws fewer of them and the
 * fire moves at the same speed; the fire itself is stepped in ticks the clock
 * owes, never more than MostOwedUs at once, so a stall does not become a leap. */
constexpr std::uint64_t FrameUs    = 33333;
constexpr std::uint64_t MostOwedUs = 8ull * FlameFire::TickUs;
constexpr int           WarmTicks  = 90;

void Say(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_to_screen(Line.c_str());
}

std::string Lower(std::string_view Word)
{
    std::string Low(Word);
    for (char &Ch : Low)
        if (Ch >= 'A' && Ch <= 'Z') Ch = static_cast<char>(Ch - 'A' + 'a');
    return Low;
}

bool TakePlacement(const std::string &Low, FlameSettings &Settings)
{
    for (int Place = 0; Place < FlamePlaceCount - 1; Place++) {
        if (Low == FlamePlaceName(static_cast<FlamePlace>(Place))) {
            Settings.Place = static_cast<FlamePlace>(Place);
            return true;
        }
    }
    return false;
}

bool TakeInk(std::string_view Word, const std::string &Low, FlameSettings &Settings)
{
    for (int Ink = 0; Ink < FlameInkCount - 1; Ink++) {
        if (Low == FlameInkName(static_cast<FlameInk>(Ink))) {
            Settings.Ink = static_cast<FlameInk>(Ink);
            return true;
        }
    }
    std::uint32_t Colour;
    if (Word.size() > 1 && Word[0] == '#' && color_parse(std::string(Word).c_str(), &Colour)) {
        Settings.Custom    = Colour;
        Settings.HasCustom = true;
        Settings.Ink       = FlameInk::Custom;
        return true;
    }
    return false;
}

/* Settings from the words; every word that is neither a placement nor a
 * colour is burned, and a word to burn means `where` is word whatever place
 * the line also names: only one thing burns at a time. */
FlameSettings ReadWords(const char *const *Words, std::size_t Count)
{
    FlameSettings Settings;
    for (std::size_t At = 0; At < Count; At++) {
        std::string Low = Lower(Words[At]);
        if (!TakePlacement(Low, Settings) && !TakeInk(Words[At], Low, Settings)) {
            if (!Settings.Word.empty()) Settings.Word += ' ';
            Settings.Word += Words[At];
        }
    }
    if (Settings.Word.size() > FlameWordMax) Settings.Word.resize(FlameWordMax);
    if (!Settings.Word.empty()) Settings.Place = FlamePlace::Word;
    if (Settings.Place == FlamePlace::Pit) Settings.Sparks = true;
    return Settings;
}

struct Show {
    painter::tui::Stage Screen;
    int                   Rows;
    int                   Cols;
    FlameSettings         Settings;
    FlamePalette          Palette;
    FlameFire             Fire;
    FlameMenu             Menu;
    std::uint32_t         Frames = 0;

    Show(const FlameSettings &Wanted, painter::tui::Stage Screen)
        : Screen(std::move(Screen)), Rows(this->Screen.Size().Height), Cols(this->Screen.Size().Width), Settings(Wanted),
          Palette(FlameMakePalette(Wanted.Ink, Wanted.Custom)),
          Fire(Cols, Rows * 2, cpu_rdtsc() * 0x9E3779B97F4A7C15ull),
          Frames(0)
    {
        Fire.Rebuild(Settings);
        for (int Tick = 0; Tick < WarmTicks; Tick++) Fire.Step(Settings);
    }

    bool Draw();
    bool Run();
    void Close() { (void)Screen.Close(); }
    bool OnKey(const painter::tui::KeyEvent &Event);
};

bool Show::OnKey(const painter::tui::KeyEvent &Event)
{
    FlameOutcome Outcome = Menu.OnKey(Event, Settings);
    if (Outcome.Rebuild) Fire.Rebuild(Settings);
    if (Outcome.Repalette) Palette = FlameMakePalette(Settings.Ink, Settings.Custom);
    return !Outcome.Leave;
}

/* The caret is parked before the paint: the frame is then the last thing to
 * reach the glass and already carries the caret where it belongs. */
bool Show::Draw()
{
    painter::tui::Frame &Canvas = Screen.FrameBuffer();
    FlamePaintFire(Fire, Palette, Canvas, Rows, Cols);
    if (Menu.Open) Menu.Draw(Canvas, Rows, Cols, Settings);

    int Row, Col;
    Menu.Caret(Rows, Cols, Row, Col);
    if (!Screen.SetCursor({Col, Row})) return false;
    if (!Screen.Present()) return false;
    Frames++;
    return true;
}

/* False if the screen stopped taking frames. The next deadline is taken after
 * the paint, so a screen so wide that one frame outlasts FrameUs still stops
 * to listen for keys; and the timed Stage wait lets the core idle. */
bool Show::Run()
{
    std::uint64_t Last = clock_uptime_us();
    std::uint64_t Next = Last;
    std::uint64_t Owed = 0;

    for (;;) {
        std::uint64_t Now = clock_uptime_us();
        if (Now >= Next) {
            Owed += std::min(Now - Last, MostOwedUs);
            Last = Now;
            for (; Owed >= FlameFire::TickUs; Owed -= FlameFire::TickUs) Fire.Step(Settings);

            if (!Draw()) return false;
            Next = clock_uptime_us() + FrameUs;
            continue;
        }

        auto  LeftMs = static_cast<std::uint32_t>((Next - Now) / 1000u);
        painter::tui::Event Event = Screen.Wait(LeftMs == 0 ? 1 : LeftMs);
        if (Event.Kind == painter::tui::EventKind::Key) {
            if (!OnKey(Event.Key)) return true;
        } else if (Event.Kind == painter::tui::EventKind::Gone) {
            return true;
        }
    }
}

std::string Tally(const Show &Fire, bool Painted)
{
    std::string Line = std::format("[FLAME] {} frames - {}, ", Fire.Frames, FlamePlaceName(Fire.Settings.Place));
    if (Fire.Settings.Ink == FlameInk::Custom) {
        char Spelled[8];
        color_format(Fire.Settings.Custom, Spelled);
        Line += Spelled;
    } else {
        Line += FlameInkName(Fire.Settings.Ink);
    }
    if (!Painted) Line += "\nThe screen stopped taking frames.";
    return Line;
}

}

int main()
{
    try {
        const char *const *Words;
        std::size_t        Count;
        if (form_words(0, &Words, &Count) != 0) return 1;
        FlameSettings Wanted = ReadWords(Words, Count);

        std::expected<painter::tui::Stage, painter::tui::StageError> Opened =
            painter::tui::Stage::Open(FlameMinCols, FlameMinRows);
        if (!Opened) {
            Say(Opened.error().Describe("flame"));
            return 1;
        }

        Show Fire(Wanted, std::move(*Opened));
        bool Painted = Fire.Run();
        Fire.Close();
        Say(Tally(Fire, Painted));
        return Painted ? 0 : 1;
    } catch (const std::bad_alloc &) {
        Say("There is not enough memory for a fire the size of this screen.");
        return 1;
    }
}
