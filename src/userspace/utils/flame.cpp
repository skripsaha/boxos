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
 * One vga_paint a frame, never a cell at a time: its header says why.
 */

#include <algorithm>
#include <cstring>
#include <format>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "box/color.h"
#include "box/cpu.h"
#include "box/clock.h"
#include "box/error.h"
#include "box/form.h"
#include "box/keyboard.h"
#include "box/print.h"
#include "box/touch.h"
#include "box/vga.h"

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
    FlameSettings         Settings;
    FlamePalette          Palette;
    FlameFire             Fire;
    FlameMenu             Menu;
    std::vector<TextCell> Frame;
    int                   Rows;
    int                   Cols;
    TouchTag              Ear = TOUCH_TAG_INVALID;
    std::uint32_t         Frames = 0;

    Show(const FlameSettings &Wanted, int Rows, int Cols)
        : Settings(Wanted), Palette(FlameMakePalette(Wanted.Ink, Wanted.Custom)),
          Fire(Cols, Rows * 2, cpu_rdtsc() * 0x9E3779B97F4A7C15ull),
          Frame(static_cast<std::size_t>(Rows) * Cols), Rows(Rows), Cols(Cols)
    {
        Fire.Rebuild(Settings);
        for (int Tick = 0; Tick < WarmTicks; Tick++) Fire.Step(Settings);
    }

    bool Draw();
    bool Run();
    bool OnKey(const kb_event_t &Event);
};

bool Show::OnKey(const kb_event_t &Event)
{
    FlameKey Key{Event.ascii, (Event.mods & KB_MOD_CTRL) != 0, (Event.mods & KB_MOD_EXTENDED) != 0, Event.scancode};
    FlameOutcome Outcome = Menu.OnKey(Key, Settings);
    if (Outcome.Rebuild) Fire.Rebuild(Settings);
    if (Outcome.Repalette) Palette = FlameMakePalette(Settings.Ink, Settings.Custom);
    return !Outcome.Leave;
}

/* The caret is parked before the paint: the frame is then the last thing to
 * reach the glass and already carries the caret where it belongs. */
bool Show::Draw()
{
    FlamePaintFire(Fire, Palette, Frame.data(), Rows, Cols);
    if (Menu.Open) Menu.Draw(Frame.data(), Rows, Cols, Settings);

    int Row, Col;
    Menu.Caret(Rows, Cols, Row, Col);
    vga_setcursor(static_cast<std::uint8_t>(Row), static_cast<std::uint8_t>(Col));
    if (vga_paint(0, 0, static_cast<std::uint8_t>(Rows), static_cast<std::uint8_t>(Cols), Frame.data()) != 0) return false;
    Frames++;
    return true;
}

/* False if the screen stopped taking frames. The next deadline is taken after
 * the paint, so a screen so wide that one frame outlasts FrameUs still stops
 * to listen for keys; and the wait is a timed touch_await, so the core idles. */
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
        Touch Event;
        int   Answer = touch_await(Ear, &Event, LeftMs == 0 ? 1 : LeftMs);
        if (Answer == 0 && Event.payload_len >= sizeof(kb_event_t)) {
            kb_event_t Key;
            std::memcpy(&Key, Event.payload, sizeof(Key));
            if (!OnKey(Key)) return true;
        } else if (Answer != 0 && Answer != -ERR_TIMEOUT) {
            return true;
        }
    }
}

/* The screen is given back however the program leaves it, before any word is
 * said: printed text lands whenever the display daemon next runs. */
struct ScreenHeld {
    ~ScreenHeld()
    {
        console_unlisten();
        vga_clear_rgb(COLOR_LIGHT_GRAY, COLOR_BLACK);
        vga_setcursor(0, 0);
    }
};

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

        vga_dimensions_t Glass;
        if (vga_getdimensions(&Glass) != 0) {
            Say("This machine did not say how big its screen is.");
            return 1;
        }
        if (Glass.rows == 0 || Glass.cols == 0) {
            Say("This machine has no console to paint on.");
            return 1;
        }
        if (Glass.cols < FlameMinCols || Glass.rows < FlameMinRows) {
            Say(std::format("This screen is {} columns by {} rows; flame needs at least {} by {}.",
                            Glass.cols, Glass.rows, FlameMinCols, FlameMinRows));
            return 1;
        }

        Show Fire(Wanted, Glass.rows, Glass.cols);
        Fire.Ear = console_listen();
        if (Fire.Ear == TOUCH_TAG_INVALID) {
            Say("This machine has no way to hear a key.");
            return 1;
        }

        bool Painted;
        {
            ScreenHeld Held;
            Painted = Fire.Run();
        }
        Say(Tally(Fire, Painted));
        return Painted ? 0 : 1;
    } catch (const std::bad_alloc &) {
        Say("There is not enough memory for a fire the size of this screen.");
        return 1;
    }
}
