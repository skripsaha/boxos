/* aquarium — a tank of water behind the glass of the console.
 *
 *   aquarium              the tank, lit as the clock says: day, dusk or night
 *   aquarium night        always night: the jellyfish and lanternfish glow
 *   aquarium 40           about forty that swim
 *
 *   arrows                look about the tank; it is wider and deeper than the glass
 *   space                 a pinch of food: who is hungry comes for it
 *   t                     a knock on the glass: they scatter, the pufferfish puffs up
 *   a                     a new fish
 *   n                     the light: by the clock, day, dusk, night
 *   h                     the line of keys, shown and hidden
 *   q, ESC                leave, and say how it went
 *
 * What lives there and how it looks is aquarium_kinds.cpp, how it moves
 * aquarium_life.cpp, what stands still aquarium_scene.cpp, the picture
 * aquarium_paint.cpp. One vga_paint a frame, as flame paints.
 */

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "box/clock.h"
#include "box/color.h"
#include "box/cpu.h"
#include "box/error.h"
#include "box/form.h"
#include "box/keyboard.h"
#include "box/print.h"
#include "box/touch.h"
#include "box/vga.h"

#include "aquarium.h"

BOX_FORM("takes optional words \"its light (day, dusk or night; by the clock if none is said) "
         "and how many fish swim in it\"\n");

namespace {

/* Frames come by the clock and life is stepped in ticks the clock owes, never
 * more than MostOwedUs at once, so a stall does not become a leap. */
constexpr std::uint64_t FrameUs     = 33333;
constexpr std::uint64_t TickUs      = 25000;
constexpr std::uint64_t MostOwedUs  = 8 * TickUs;
constexpr float         TickSeconds = static_cast<float>(TickUs) / 1e6f;
constexpr float         KeysShownSeconds = 8.0f;
constexpr int           MostFish    = 500;
constexpr int           LeastCols   = 40;
constexpr int           LeastRows   = 12;

constexpr char ArrowLeft  = 0x1B;   /* the console font's arrows */
constexpr char ArrowRight = 0x1A;
constexpr char ArrowUp    = 0x18;
constexpr char ArrowDown  = 0x19;
constexpr char FullBlock  = static_cast<char>(0xDB);
constexpr char LightShade = static_cast<char>(0xB0);

/* Exact greys and saturated hues, so the bar reads the same on a text console
 * (typer_glass.h spells out why). */
const Color BarPaper = COLOR_RGB(0x3A, 0x3A, 0x3A);
const Color BarInk   = COLOR_RGB(0xEE, 0xEE, 0xEE);
const Color KeyInk   = COLOR_AMBER;
const Color HintInk  = COLOR_RGB(0x8C, 0x8C, 0x8C);

void Say(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_to_screen(Line.c_str());
}

struct Wanted {
    AquaLight Light = AquaLight::Clock;
    int       Fish  = 0;
};

/* The words: a light, a number of fish; a word that is neither is said back. */
bool ReadWords(const char *const *Words, std::size_t Count, Wanted &Want)
{
    for (std::size_t At = 0; At < Count; At++) {
        std::string_view Word = Words[At];
        int              Number = 0;
        auto [End, Fault] = std::from_chars(Word.data(), Word.data() + Word.size(), Number);
        if (Fault == std::errc() && End == Word.data() + Word.size()) {
            if (Number < 1 || Number > MostFish) {
                Say(std::format("aquarium: {} fish will not do; one to {} will.", Word, MostFish));
                return false;
            }
            Want.Fish = Number;
            continue;
        }
        bool Known = false;
        for (AquaLight Light : {AquaLight::Day, AquaLight::Dusk, AquaLight::Night}) {
            if (Word == AquaLightName(Light)) {
                Want.Light = Light;
                Known      = true;
            }
        }
        if (!Known) {
            Say(std::format("aquarium: what is {}? It takes day, dusk or night, and a number of fish.", Word));
            return false;
        }
    }
    return true;
}

struct Show {
    AquaDice              Dice;
    AquaTank              Tank;
    AquaLight             Light;
    std::vector<Color>    Pixels;
    std::vector<TextCell> Frame;
    int                   Rows, Cols;
    float                 X = 0, Y = 0;           /* the window's top-left in the tank */
    float                 GoalX = 0, GoalY = 0;   /* where the arrows sent it */
    float                 KeysUntil = KeysShownSeconds;
    TouchTag              Ear = TOUCH_TAG_INVALID;
    std::uint32_t         Frames = 0;
    int                   Hour = 12, Minute = 0;
    float                 ClockRead = -1.0f;

    Show(const Wanted &Want, int Rows, int Cols)
        : Dice(cpu_rdtsc() * 0x9E3779B97F4A7C15ull), Tank(AquaMakeTank(Cols, Rows * 2, Want.Fish, Dice)),
          Light(Want.Light), Frame(static_cast<std::size_t>(Rows) * Cols), Rows(Rows), Cols(Cols)
    {
        GoalX = X = static_cast<float>(Tank.Width - Cols) / 2.0f;
        GoalY = Y = static_cast<float>(Tank.Height - Rows * 2);
    }

    float MostX() const { return static_cast<float>(Tank.Width - Cols); }
    float MostY() const { return static_cast<float>(Tank.Height - Rows * 2); }
    float MiddleX() const { return X + static_cast<float>(Cols) / 2.0f; }
    float MiddleY() const { return Y + static_cast<float>(Rows); }

    void         Step();
    AquaDaylight Daylight();
    void         Bar();
    bool         Draw();
    bool         OnKey(const kb_event_t &Key);
    bool         Run();
};

/* A tick of life, and the window easing toward where the arrows sent it. */
void Show::Step()
{
    AquaStep(Tank, TickSeconds, Dice);
    float Ease = std::min(1.0f, TickSeconds * 6.0f);
    X += (GoalX - X) * Ease;
    Y += (GoalY - Y) * Ease;
}

/* The light now; the clock is read once a second, not every frame. */
AquaDaylight Show::Daylight()
{
    if (Light == AquaLight::Clock && (ClockRead < 0 || Tank.Time - ClockRead >= 1.0f)) {
        BoxTime Now;
        if (clock_boxtime_local(&Now) == 0) {
            Hour   = Now.hour;
            Minute = Now.minute;
        }
        ClockRead = Tank.Time;
    }
    return AquaDaylightAt(Light, Hour, Minute);
}

int Write(TextCell *Row, int Cols, int Col, std::string_view Text, Color Ink, Color Paper)
{
    for (char Ch : Text) {
        if (Col >= Cols) break;
        Row[Col++] = TextCell{Ink, Paper, Ch, {0, 0, 0}};
    }
    return Col;
}

/* The line of keys at the bottom. Each piece is laid whole or not at all, in
 * the order it is needed: the keys, how many live, where in the tank the
 * window stands — so a narrow screen loses the last, never half a word. */
void Show::Bar()
{
    TextCell *Row = &Frame[static_cast<std::size_t>(Rows - 1) * Cols];
    for (int Col = 0; Col < Cols; Col++) Row[Col] = TextCell{BarInk, BarPaper, ' ', {0, 0, 0}};
    auto Fits = [this](int Col, std::size_t Length) { return Col + static_cast<int>(Length) < Cols; };

    int Col = Write(Row, Cols, 1, "aquarium", BarInk, BarPaper);
    Col     = Write(Row, Cols, Col + 1, AquaLightName(Light), HintInk, BarPaper);

    /* How to leave is never what a narrow screen loses: its room is kept
     * while the others are laid. */
    struct Key { std::string Keys; const char *What; };
    const Key   Keys[] = {{{ArrowLeft, ArrowRight, ArrowUp, ArrowDown}, "look"}, {"space", "feed"}, {"t", "knock"},
                          {"a", "fish"}, {"n", "light"}, {"h", "hide"}};
    const Key   Leave{"q", "leave"};
    auto        Width = [](const Key &Each) { return 2 + Each.Keys.size() + 1 + std::strlen(Each.What); };
    bool        Full  = false;
    for (const Key &Each : Keys) {
        Full = Full || !Fits(Col, Width(Each) + Width(Leave));
        if (Full) continue;
        Col = Write(Row, Cols, Col + 2, Each.Keys, KeyInk, BarPaper);
        Col = Write(Row, Cols, Col + 1, Each.What, HintInk, BarPaper);
    }
    if (!Fits(Col, Width(Leave))) return;
    Col = Write(Row, Cols, Col + 2, Leave.Keys, KeyInk, BarPaper);
    Col = Write(Row, Cols, Col + 1, Leave.What, HintInk, BarPaper);
    if (Full) return;

    std::size_t Living = 0;
    for (const AquaCreature &Creature : Tank.Creatures) Living += !Creature.Gone;
    std::string Count = std::format("{} living", Living);
    if (!Fits(Col + 3, Count.size())) return;
    Col = Write(Row, Cols, Col + 3, Count, BarInk, BarPaper);

    /* The tank's width in eight steps, the window's step filled. */
    constexpr int Steps = 8;
    if (!Fits(Col + 2, Steps + 1)) return;
    int At = MostX() > 0 ? static_cast<int>(X / MostX() * (Steps - 1) + 0.5f) : 0;
    for (int Step = 0; Step < Steps; Step++)
        Row[Cols - Steps - 1 + Step] = TextCell{Step == At ? BarInk : HintInk, BarPaper,
                                                Step == At ? FullBlock : LightShade, {0, 0, 0}};
}

bool Show::Draw()
{
    AquaView View{Cols, Rows * 2, X, Y};
    AquaPaint(Tank, View, Daylight(), Pixels);
    AquaCells(Pixels, Cols, Frame.data(), Rows, Cols);
    if (Tank.Time < KeysUntil) Bar();
    vga_setcursor(static_cast<std::uint8_t>(Rows - 1), static_cast<std::uint8_t>(Cols - 1));
    if (vga_paint(0, 0, static_cast<std::uint8_t>(Rows), static_cast<std::uint8_t>(Cols), Frame.data()) != 0) return false;
    Frames++;
    return true;
}

/* False to leave. */
bool Show::OnKey(const kb_event_t &Key)
{
    if (Key.mods & KB_MOD_EXTENDED) {
        float StepX = static_cast<float>(Cols) / 4.0f, StepY = static_cast<float>(Rows) / 2.0f;
        switch (Key.scancode) {
        case KEY_LEFT:  GoalX = std::max(0.0f, GoalX - StepX); break;
        case KEY_RIGHT: GoalX = std::min(MostX(), GoalX + StepX); break;
        case KEY_UP:    GoalY = std::max(0.0f, GoalY - StepY); break;
        case KEY_DOWN:  GoalY = std::min(MostY(), GoalY + StepY); break;
        default: break;
        }
        return true;
    }
    switch (Key.ascii) {
    case ' ': AquaFeed(Tank, MiddleX(), Dice); break;
    case 't':
    case 'T': AquaTap(Tank, MiddleX(), MiddleY()); break;
    case 'a':
    case 'A': AquaAdd(Tank, MiddleX(), MiddleY(), Dice); break;
    case 'n':
    case 'N':
        Light     = static_cast<AquaLight>((static_cast<int>(Light) + 1) % 4);
        ClockRead = -1.0f;
        KeysUntil = std::max(KeysUntil, Tank.Time + 2.0f);
        break;
    case 'h':
    case 'H':
    case '?': KeysUntil = Tank.Time < KeysUntil ? 0.0f : 1e30f; break;
    case 'q':
    case 'Q':
    case KEY_ESCAPE: return false;
    default: break;
    }
    return true;
}

/* False if the screen stopped taking frames. The wait between frames is a
 * timed touch_await, so the core rests and a key is heard at once. */
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
            for (; Owed >= TickUs; Owed -= TickUs) Step();
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

std::string Tally(const Show &Tank, bool Painted)
{
    std::size_t Living = 0;
    for (const AquaCreature &Creature : Tank.Tank.Creatures) Living += !Creature.Gone;
    std::string Line = std::format("[AQUARIUM] {} frames - {} living, fed {} {}", Tank.Frames, Living, Tank.Tank.Fed,
                                   Tank.Tank.Fed == 1 ? "time" : "times");
    if (!Painted) Line += "\nThe screen stopped taking frames.";
    return Line;
}

}

const char *AquaLightName(AquaLight Light)
{
    switch (Light) {
    case AquaLight::Clock: return "clock";
    case AquaLight::Day:   return "day";
    case AquaLight::Dusk:  return "dusk";
    case AquaLight::Night: return "night";
    }
    return "clock";
}

int main()
{
    try {
        const char *const *Words;
        std::size_t        Count;
        if (form_words(0, &Words, &Count) != 0) return 1;
        Wanted Want;
        if (!ReadWords(Words, Count, Want)) return 1;

        vga_dimensions_t Glass;
        if (vga_getdimensions(&Glass) != 0) {
            Say("This machine did not say how big its screen is.");
            return 1;
        }
        if (Glass.rows == 0 || Glass.cols == 0) {
            Say("This machine has no console to paint on.");
            return 1;
        }
        if (Glass.cols < LeastCols || Glass.rows < LeastRows) {
            Say(std::format("This screen is {} columns by {} rows; aquarium needs at least {} by {}.",
                            Glass.cols, Glass.rows, LeastCols, LeastRows));
            return 1;
        }

        AquaMakeWaves();
        Show Tank(Want, Glass.rows, Glass.cols);
        Tank.Ear = console_listen();
        if (Tank.Ear == TOUCH_TAG_INVALID) {
            Say("This machine has no way to hear a key.");
            return 1;
        }

        bool Painted;
        {
            ScreenHeld Held;
            Painted = Tank.Run();
        }
        Say(Tally(Tank, Painted));
        return Painted ? 0 : 1;
    } catch (const std::bad_alloc &) {
        Say("There is not enough memory for a tank the size of this screen.");
        return 1;
    }
}
