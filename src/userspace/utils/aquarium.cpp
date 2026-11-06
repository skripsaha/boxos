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
 * aquarium_paint.cpp. Painter presents one composed frame per animation step.
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
#include "box/cpu.h"
#include "box/error.h"
#include "box/form.h"
#include "box/print.h"

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
 * (typer_screen.h spells out why). */
const AquaColor BarPaper = AquaColor::Rgb(0x3A, 0x3A, 0x3A);
const AquaColor BarInk   = AquaColor::Rgb(0xEE, 0xEE, 0xEE);
const AquaColor KeyInk   = painter::core::Colors::Amber;
const AquaColor HintInk  = AquaColor::Rgb(0x8C, 0x8C, 0x8C);

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
    painter::tui::Stage    Screen;
    int                    Rows, Cols;
    AquaDice              Dice;
    AquaTank              Tank;
    AquaLight             Light;
    std::vector<AquaColor> Pixels;
    float                 X = 0, Y = 0;           /* the window's top-left in the tank */
    float                 GoalX = 0, GoalY = 0;   /* where the arrows sent it */
    float                 KeysUntil = KeysShownSeconds;
    std::uint32_t         Frames = 0;
    int                   Hour = 12, Minute = 0;
    float                 ClockRead = -1.0f;

    Show(const Wanted &Want, painter::tui::Stage Display)
        : Screen(std::move(Display)), Rows(Screen.Size().Height), Cols(Screen.Size().Width),
          Dice(cpu_rdtsc() * 0x9E3779B97F4A7C15ull), Tank(AquaMakeTank(Cols, Rows * 2, Want.Fish, Dice)),
          Light(Want.Light)
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
    void         Bar(painter::tui::Frame &Canvas);
    bool         Draw();
    bool         OnKey(const painter::tui::KeyEvent &Key);
    bool         Run();
    void         Close() { (void)Screen.Close(); }
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

int Write(painter::tui::Frame &Canvas, int Cols, int Row, int Col, std::string_view Text, AquaColor Ink,
          AquaColor Paper)
{
    for (char Ch : Text) {
        if (Col >= Cols) break;
        Canvas.Put({Col++, Row}, painter::tui::Cell{Ch, Ink, Paper});
    }
    return Col;
}

/* The line of keys at the bottom. Each piece is laid whole or not at all, in
 * the order it is needed: the keys, how many live, where in the tank the
 * window stands — so a narrow screen loses the last, never half a word. */
void Show::Bar(painter::tui::Frame &Canvas)
{
    for (int Col = 0; Col < Cols; Col++) Canvas.Put({Col, Rows - 1}, painter::tui::Cell{' ', BarInk, BarPaper});
    auto Fits = [this](int Col, std::size_t Length) { return Col + static_cast<int>(Length) < Cols; };

    int Col = Write(Canvas, Cols, Rows - 1, 1, "aquarium", BarInk, BarPaper);
    Col     = Write(Canvas, Cols, Rows - 1, Col + 1, AquaLightName(Light), HintInk, BarPaper);

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
        Col = Write(Canvas, Cols, Rows - 1, Col + 2, Each.Keys, KeyInk, BarPaper);
        Col = Write(Canvas, Cols, Rows - 1, Col + 1, Each.What, HintInk, BarPaper);
    }
    if (!Fits(Col, Width(Leave))) return;
    Col = Write(Canvas, Cols, Rows - 1, Col + 2, Leave.Keys, KeyInk, BarPaper);
    Col = Write(Canvas, Cols, Rows - 1, Col + 1, Leave.What, HintInk, BarPaper);
    if (Full) return;

    std::size_t Living = 0;
    for (const AquaCreature &Creature : Tank.Creatures) Living += !Creature.Gone;
    std::string Count = std::format("{} living", Living);
    if (!Fits(Col + 3, Count.size())) return;
    Col = Write(Canvas, Cols, Rows - 1, Col + 3, Count, BarInk, BarPaper);

    /* The tank's width in eight steps, the window's step filled. */
    constexpr int Steps = 8;
    if (!Fits(Col + 2, Steps + 1)) return;
    int At = MostX() > 0 ? static_cast<int>(X / MostX() * (Steps - 1) + 0.5f) : 0;
    for (int Step = 0; Step < Steps; Step++)
        Canvas.Put({Cols - Steps - 1 + Step, Rows - 1},
                   painter::tui::Cell{Step == At ? FullBlock : LightShade, Step == At ? BarInk : HintInk, BarPaper});
}

bool Show::Draw()
{
    AquaView View{Cols, Rows * 2, X, Y};
    AquaPaint(Tank, View, Daylight(), Pixels);
    painter::tui::Frame &Canvas = Screen.FrameBuffer();
    AquaCells(Pixels, Cols, Canvas, Rows, Cols);
    if (Tank.Time < KeysUntil) Bar(Canvas);
    if (!Screen.SetCursor({Cols - 1, Rows - 1})) return false;
    if (!Screen.Present()) return false;
    Frames++;
    return true;
}

/* False to leave. */
bool Show::OnKey(const painter::tui::KeyEvent &Key)
{
    if (Key.IsExtended()) {
        float StepX = static_cast<float>(Cols) / 4.0f, StepY = static_cast<float>(Rows) / 2.0f;
        switch (Key.Scancode) {
        case painter::tui::KeyCodes::Left:  GoalX = std::max(0.0f, GoalX - StepX); break;
        case painter::tui::KeyCodes::Right: GoalX = std::min(MostX(), GoalX + StepX); break;
        case painter::tui::KeyCodes::Up:    GoalY = std::max(0.0f, GoalY - StepY); break;
        case painter::tui::KeyCodes::Down:  GoalY = std::min(MostY(), GoalY + StepY); break;
        default: break;
        }
        return true;
    }
    switch (Key.Ascii) {
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
    case painter::tui::KeyCodes::Escape: return false;
    default: break;
    }
    return true;
}

/* False if the screen stopped taking frames. The timed Stage wait lets the
 * core rest and still hears a key at once. */
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
        painter::tui::Event Event = Screen.Wait(LeftMs == 0 ? 1 : LeftMs);
        if (Event.Kind == painter::tui::EventKind::Key) {
            if (!OnKey(Event.Key)) return true;
        } else if (Event.Kind == painter::tui::EventKind::Gone) {
            return true;
        }
    }
}

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

        std::expected<painter::tui::Stage, painter::tui::StageError> Opened =
            painter::tui::Stage::Open("aquarium", LeastCols, LeastRows);
        if (!Opened) {
            Say(Opened.error().Describe("aquarium"));
            return 1;
        }

        AquaMakeWaves();
        Show Tank(Want, std::move(*Opened));
        bool Painted = Tank.Run();
        Tank.Close();
        Say(Tally(Tank, Painted));
        return Painted ? 0 : 1;
    } catch (const std::bad_alloc &) {
        Say("There is not enough memory for a tank the size of this screen.");
        return 1;
    }
}
