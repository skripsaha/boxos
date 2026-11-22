/* game.cpp — the frames, the keys and the rounds one after another. See
 * game.h. */

#include "game.h"

#include <algorithm>
#include <charconv>
#include <format>

#include "box/alarm.h"
#include "box/clock.h"
#include "box/error.h"
#include "box/time.h"
#include "keep.h"

namespace flappybird {

namespace {

namespace KeyCodes = painter::tui::KeyCodes;

const Keep Record{"flappybird.record", "flappybird,record"};

constexpr std::uint64_t CountFromUs = 1350000;   // the board's score counts up once the board is in
constexpr std::uint64_t CountStepUs = 45000;     // a point each
constexpr std::uint64_t AgainAfterUs = 1500000;  // a flap begins a new round once the board says how

/* Night when the machine's clock is past seven in the evening or before seven
 * in the morning. */
const Sky &SkyNow()
{
    BoxTime Now{};
    if (clock_boxtime_local(&Now) != OK) return Day;
    return Now.hour >= 7 && Now.hour < 19 ? Day : Night;
}

unsigned ReadBest(std::string &Problem)
{
    std::string Text;
    if (std::optional<std::string> Refusal = KeepRead(Record, Text)) {
        Problem = *Refusal;
        return 0;
    }
    unsigned          Best = 0;
    const std::size_t At   = Text.find("best ");
    if (At != std::string::npos) std::from_chars(Text.data() + At + 5, Text.data() + Text.size(), Best);
    return Best;
}

}  // namespace

Game::Game(gui::Area Opened, painter::tui::Keyboard Heard) : Screen(std::move(Opened)), Keys(std::move(Heard))
{
    Best = ReadBest(Problem);
}

void Game::Build()
{
    Words.reset();
    Picture.reset();
    BuiltFor        = Screen.Size();
    const float Unit = BuiltFor.Y / field::Tall;
    Field.emplace(BuiltFor.X / Unit);
    Picture.emplace(Screen, *Field, SkyNow());
    Words.emplace(Screen, Unit, BuiltFor.X);
    Words->Ready(Best);
    Paused = Closing = false;
    Counted          = 0;
}

void Game::Run()
{
    Keys.HearEnd();
    Build();
    LastUs = clock_uptime_us();
    std::uint64_t Seen = 0;   // the last step the glass showed
    for (;;) {
        const std::uint64_t Before = Screen.Step();
        if (Moving()) Step();
        CountUp();
        (void)Screen.Present();
        /* A frame said: the next once the glass shows it. Nothing moving,
         * only what comes next — a key, the display's word — wakes it. */
        const bool Framed = Screen.Step() != Before && Moving();
        /* What the display told and what was typed meanwhile taken first —
         * the display tells something every frame, and keys waited for only
         * behind it would wait long — then the next thing waited for. */
        bool Woken = false;
        for (;;) {
            while (const std::optional<gui::Event> Told = Screen.Next()) {
                if (!OnEvent(*Told, Seen)) return;
                Woken = true;
            }
            for (painter::tui::Event Typed = Keys.Poll(); Typed.Kind != painter::tui::EventKind::Quiet; Typed = Keys.Poll()) {
                if (Typed.Kind != painter::tui::EventKind::Key || !OnKey(Typed.Key)) return;
                Woken = true;
            }
            if (Framed ? Seen >= Screen.Step() : Woken) break;
            if (!OnEvent(Screen.Wait(&Keys), Seen)) return;
            Woken = true;
        }
    }
}

/* False to leave. */
bool Game::OnEvent(const gui::Event &Told, std::uint64_t &Seen)
{
    switch (Told.What) {
    case gui::Event::Kind::Gone:
    case gui::Event::Kind::EndAsked: return false;
    case gui::Event::Kind::Key: return OnKey(Told.Key);
    case gui::Event::Kind::Area: OnArea(Told); break;
    case gui::Event::Kind::Shown: Seen = std::max(Seen, Told.Step); break;
    case gui::Event::Kind::AnimationEnd:
        if (Told.Number == CurtainClosed) Again();
        break;
    default: break;
    }
    return true;
}

bool Game::Moving() const
{
    return !Hidden && !Paused && Field && Field->Now() != Stage::Over;
}

/* Over, the board's score counts up a point at a time once the board is in,
 * the alarm waking the game for each. */
void Game::CountUp()
{
    if (Field->Now() != Stage::Over || Counted >= Field->Score()) return;
    const auto          Due = [&](unsigned Point) { return OverUs + CountFromUs + (Point - 1) * CountStepUs; };
    const std::uint64_t Now = clock_uptime_us();
    if (Now >= Due(Counted + 1)) {
        Counted = std::min(Field->Score(), static_cast<unsigned>((Now - OverUs - CountFromUs) / CountStepUs) + 1);
        Words->Count(Counted);
    }
    if (Counted < Field->Score()) (void)alarm_set_us(Due(Counted + 1));
}

void Game::Step()
{
    const std::uint64_t Now = clock_uptime_us();
    const float         Gone = static_cast<float>(Now - LastUs) / 1e6f;
    LastUs = Now;
    const Happened What = Field->Advance(Gone);
    if (What.Scored) Words->Score(Field->Score());
    if (What.Struck) Words->Flash();
    if (What.Landed) Land();
    Picture->Lay(*Field);
}

void Game::Flap()
{
    switch (Field->Now()) {
    case Stage::Ready:
        Field->Flap();
        Picture->Lay(*Field);
        Picture->ShowPipes(*Field);
        Words->Flying();
        break;
    case Stage::Flying:
        if (!Paused) Field->Flap();
        break;
    case Stage::Over:
        if (!Closing && clock_uptime_us() >= OverUs + AgainAfterUs) {
            Closing = true;
            Words->CloseCurtain();
        }
        break;
    case Stage::Falling: break;
    }
}

/* Down on the ground: the board, and the record kept when it was beaten. */
void Game::Land()
{
    const unsigned Score = Field->Score();
    const bool     New   = Score > Best;
    if (New) {
        Best = Score;
        if (std::optional<std::string> Refusal = KeepWrite(Record, std::format("best {}\n", Best))) Problem = *Refusal;
    }
    OverUs  = clock_uptime_us();
    Counted = 0;
    Words->Over(Score, Best, New);
}

/* The curtain closed: a new round behind it, and it opens. */
void Game::Again()
{
    Field->Begin();
    Picture->Lay(*Field);
    Words->Ready(Best);
    Words->OpenCurtain();
    Closing = false;
    Counted = 0;
    LastUs  = clock_uptime_us();
}

void Game::SetPaused(bool On)
{
    if (Paused == On) return;
    Paused = On;
    Words->Paused(On);
    LastUs = clock_uptime_us();
}

bool Game::OnKey(const painter::tui::KeyEvent &Key)
{
    if (Key.IsExtended()) {
        if (Key.Scancode == KeyCodes::Up) Flap();
        return true;
    }
    switch (Key.Character) {
    case KeyCodes::Escape:
    case U'q':
    case U'Q': return false;
    case U' ':
    case KeyCodes::Enter:
    case U'w':
    case U'W': Flap(); break;
    case U'p':
    case U'P':
        if (Field->Now() == Stage::Flying) SetPaused(!Paused);
        break;
    default: break;
    }
    return true;
}

/* The screen changed size — a new world for it — or the game went behind
 * another program: a round in flight waits, paused, for the person. */
void Game::OnArea(const gui::Event &Told)
{
    if (Told.Size.X != BuiltFor.X || Told.Size.Y != BuiltFor.Y) {
        Build();
        LastUs = clock_uptime_us();
    }
    Hidden = !Told.Seen;
    if (Hidden && Field->Now() == Stage::Flying) SetPaused(true);
    LastUs = clock_uptime_us();
}

}  // namespace flappybird
