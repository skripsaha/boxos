/* slots.cpp — the slot machine's rules, motion and keys (slots.h). */

#include "slots.h"

#include <algorithm>
#include <cmath>
#include <format>

#include "chips.h"
#include "common.h"
#include "trig.h"

namespace casino {

namespace {

namespace tui = painter::tui;

// Symbols a second at full speed, and at the end of the brake.
constexpr float Cruise = 22.0f;
constexpr float Slow   = 3.5f;
// The brake lasts this long and ends this far short of the stop; a spring then
// takes the reel the rest of the way, and past it a little, and back.
constexpr float BrakeSeconds = 0.45f;
constexpr float ShortOfStop  = 0.45f;
constexpr float SpringOmega  = 16.0f;
constexpr float SpringZeta   = 0.55f;
constexpr float SettleSeconds = 0.8f;
// When the first reel may begin to brake, and how long after the one before
// has stopped the next may.
constexpr float FirstBrake = 0.8f;
constexpr float NextBrake  = 0.25f;
constexpr std::uint64_t JackpotUs = 7000000;
// The bulbs chase at this calm pace while the machine waits.
constexpr std::uint32_t LightsMs = 400;

bool Plain(const tui::KeyEvent &Key)
{
    return !Key.IsExtended() && !Key.IsCtrl() && !Key.IsAlt();
}

}  // namespace

Slots::Slots(Context &Shared) : Ctx(Shared)
{
    for (int Reel = 0; Reel < ReelCount; Reel++) {
        Rests[static_cast<std::size_t>(Reel)]         = static_cast<int>(Ctx.Chance.Below(StripLength));
        Drums[static_cast<std::size_t>(Reel)].Target  = static_cast<float>(Rests[static_cast<std::size_t>(Reel)]);
    }
}

std::int64_t Slots::Bet() const
{
    return ChipValue(Ctx.Shown, Size);
}

float Slots::Seconds(std::uint64_t NowUs) const
{
    return static_cast<float>(NowUs - PulledUs) / 1e6f * static_cast<float>(Ctx.Shown.Speed) / 100.0f;
}

float Slots::EndsAt() const
{
    return Drums[ReelCount - 1].BrakeAt + BrakeSeconds + SettleSeconds;
}

bool Slots::Celebrating(std::uint64_t NowUs) const
{
    return Jackpot && NowUs - SettledUs < JackpotUs;
}

// Where the reel is on the strip, in symbols, Time seconds after the pull.
float Slots::Position(int Reel, float Time) const
{
    const Drum &One = Drums[static_cast<std::size_t>(Reel)];
    if (Time < One.BrakeAt) return One.Start + Cruise * Time;
    const float Braking = Time - One.BrakeAt;
    if (Braking < BrakeSeconds)
        return One.Start + Cruise * One.BrakeAt + Cruise * Braking - (Cruise - Slow) * Braking * Braking / (2.0f * BrakeSeconds);
    const float Resting = Braking - BrakeSeconds;
    if (Resting >= SettleSeconds) return One.Target;
    static const float Damped = SpringOmega * std::sqrt(1.0f - SpringZeta * SpringZeta);
    const float        Decay  = std::exp(-SpringZeta * SpringOmega * Resting);
    const float        Short  = -ShortOfStop;
    const float        Swing  = Short * Cosine(Damped * Resting) + (Slow + SpringZeta * SpringOmega * Short) / Damped * Sine(Damped * Resting);
    return One.Target + Decay * Swing;
}

// How fast it is going, in symbols a second.
float Slots::Pace(int Reel, float Time) const
{
    return std::fabs(Position(Reel, Time + 0.01f) - Position(Reel, Time)) / 0.01f;
}

// The reel's motion worked out backwards from its stop: it must brake no
// sooner than Earliest, and brake so that it comes to the stop it was dealt.
void Slots::Plan(int Reel, float Earliest)
{
    Drum &One    = Drums[static_cast<std::size_t>(Reel)];
    One.Start    = static_cast<float>(Rests[static_cast<std::size_t>(Reel)]);
    One.Stop     = static_cast<int>(Ctx.Chance.Below(StripLength));
    const float Brake  = (Cruise + Slow) / 2.0f * BrakeSeconds;
    int         Target = static_cast<int>(std::ceil(One.Start + Cruise * Earliest + ShortOfStop + Brake));
    Target += ((One.Stop - Target) % StripLength + StripLength) % StripLength;
    One.Target  = static_cast<float>(Target);
    One.BrakeAt = (One.Target - ShortOfStop - Brake - One.Start) / Cruise;
}

void Slots::Pull(std::uint64_t NowUs)
{
    if (Running) {
        PulledUs = NowUs - static_cast<std::uint64_t>(EndsAt() * 1e8f / static_cast<float>(Ctx.Shown.Speed));
        return;
    }
    if (!Ctx.Chips.Take(Stake())) {
        Ctx.Say("Not enough chips for this bet: minus lowers it, l changes the lines.", Mood::Bad);
        return;
    }
    Staked = Stake();
    for (int Reel = 0; Reel < ReelCount; Reel++)
        Plan(Reel, Reel == 0 ? FirstBrake : Drums[static_cast<std::size_t>(Reel - 1)].BrakeAt + BrakeSeconds + NextBrake);
    Running  = true;
    Jackpot  = false;
    Paid     = 0;
    PulledUs = NowUs;
    Wins.clear();
    Ctx.Say("The reels are spinning.");
}

void Slots::Settle(std::uint64_t NowUs)
{
    for (int Reel = 0; Reel < ReelCount; Reel++) Rests[static_cast<std::size_t>(Reel)] = Drums[static_cast<std::size_t>(Reel)].Stop;
    Wins    = Evaluate(WindowAt(Rests), Lines);
    Paid    = 0;
    Jackpot = false;
    for (const Win &One : Wins) {
        Paid += One.Multiple * Bet();
        Jackpot = Jackpot || One.Jackpot;
    }
    Ctx.Chips.Settle(Staked, Paid, Jackpot);
    Running   = false;
    SettledUs = NowUs;
    if (Jackpot) {
        Coins.Begin(160, 6.0f);
        Ctx.Say(std::format("JACKPOT! Three sevens: {} chips!", Counted(Paid)), Mood::Good);
    } else if (Paid > 0) {
        if (Paid >= Staked * 5) Coins.Begin(30, 1.5f);
        Ctx.Say(std::format("{} {} paid: {} chips{}.", Wins.size(), Wins.size() == 1 ? "line" : "lines", Counted(Paid),
                            Paid > Staked ? std::format(", {} more than the stake", Counted(Paid - Staked)) : std::string(", less than the stake")),
                Paid > Staked ? Mood::Good : Mood::Plain);
    } else {
        Ctx.Say(std::format("No win: the {} chips of the stake stay with the house.", Counted(Staked)), Mood::Bad);
    }
}

void Slots::Advance(std::uint64_t NowUs)
{
    const float Passed = std::min(static_cast<float>(NowUs - SteppedUs) / 1e6f, 0.1f);
    SteppedUs          = NowUs;
    if (Coins.Falling()) Coins.Step(Passed, GlassWide, GlassHigh, Ctx.Chance);
    if (Running && Seconds(NowUs) >= EndsAt()) Settle(NowUs);
}

void Slots::ChangeBet(int By)
{
    if (Running) return;
    Size = std::clamp(Size + By, 0, ChipSizes - 1);
    Ctx.Say(std::format("The bet is {} a line, {} in all.", Counted(Bet()), Counted(Stake())));
}

void Slots::ChangeLines()
{
    if (Running) return;
    Lines = Lines == 1 ? 3 : Lines == 3 ? MostLines : 1;
    Ctx.Say(std::format("{} {} played: {} in all.", Lines, Lines == 1 ? "line" : "lines", Counted(Stake())));
}

bool Slots::OnKey(const tui::KeyEvent &Key, std::uint64_t NowUs)
{
    const kb_event_t Raw{Key.Scancode, Key.Ascii, Key.Mods};
    if (KeyChordMatches(&Ctx.Shown.Keys[KeySpin], &Raw)) {
        Pull(NowUs);
        return true;
    }
    if (!Plain(Key)) return false;
    switch (Key.Ascii) {
    case ' ':
    case tui::KeyCodes::Enter:
    case '\r': Pull(NowUs); return true;
    case '+':
    case '=':  ChangeBet(1); return true;
    case '-':
    case '_':  ChangeBet(-1); return true;
    case 'l':
    case 'L':  ChangeLines(); return true;
    default:   return false;
    }
}

std::uint32_t Slots::FrameMs(std::uint64_t NowUs) const
{
    if (Running || Coins.Falling()) return 33;
    return Celebrating(NowUs) ? 100 : LightsMs;
}

void Slots::Leave()
{
    if (Running) Settle(PulledUs);
}

std::vector<std::string> Slots::Keys() const
{
    return {Entry(std::format("space, {}", Spelled(Ctx.Shown.Keys[KeySpin])), "pull the lever; again, to hurry the reels"),
            Entry("enter", "pull the lever"),
            Entry("+ and -", "the bet on each line: bigger, smaller"),
            Entry("l", "lines played: 1, 3 or 5")};
}

}  // namespace casino
