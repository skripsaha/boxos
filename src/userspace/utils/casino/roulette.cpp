/* roulette.cpp — the roulette table's rules and keys (roulette.h). */

#include "roulette.h"

#include <algorithm>
#include <format>

#include "chips.h"
#include "common.h"

namespace casino {

namespace {

namespace tui = painter::tui;

constexpr std::size_t MostResults = 40;

bool Plain(const tui::KeyEvent &Key)
{
    return !Key.IsExtended() && !Key.IsCtrl() && !Key.IsAlt();
}

}  // namespace

std::int64_t Roulette::ChipNow() const
{
    return ChipValue(Ctx.Shown, Size);
}

float Roulette::Seconds(std::uint64_t NowUs) const
{
    return static_cast<float>(NowUs - ThrownUs) / 1e6f * static_cast<float>(Ctx.Shown.Speed) / 100.0f;
}

void Roulette::Advance(std::uint64_t NowUs)
{
    if (!Turning) return;
    const float Elapsed = Seconds(NowUs);
    Last                = Turning->At(Elapsed);
    if (!Settled && Elapsed >= Turning->LandsAt()) Settle();
    if (Elapsed >= Turning->EndsAt()) {
        Last    = Turning->At(Turning->EndsAt());
        EndedUs = NowUs;
        Turning.reset();
    }
}

void Roulette::Throw(std::uint64_t NowUs)
{
    if (Turning) {
        ThrownUs = NowUs - static_cast<std::uint64_t>(Turning->EndsAt() * 1e8f / static_cast<float>(Ctx.Shown.Speed));
        return;
    }
    if (Laid.Empty()) {
        Ctx.Say("Lay a chip on the board first: enter puts one on the place under the cursor.");
        return;
    }
    Turning.emplace(static_cast<int>(Ctx.Chance.Below(Pockets)), Ctx.Chance);
    ThrownUs  = NowUs;
    Settled   = false;
    Number    = -1;
    Splitting = false;
    Ctx.Say("No more bets: the ball is thrown.");
}

void Roulette::Settle()
{
    Number                       = Turning->Result();
    const std::int64_t Staked    = Laid.Total();
    const std::int64_t Returned  = Laid.Returned(Number);
    const std::int64_t Net       = Returned - Staked;
    Ctx.Chips.Settle(Staked, Returned, false);
    Results.push_back(Number);
    if (Results.size() > MostResults) Results.erase(Results.begin());
    Before  = Laid;
    Settled = true;
    Laid.Clear();
    const std::string Word = std::format("{} {}", Number, HueName(HueOf(Number)));
    if (Net > 0) Ctx.Say(std::format("{}: you win {} chips.", Word, Counted(Net)), Mood::Good);
    else if (Net < 0) Ctx.Say(std::format("{}: you lose {} chips.", Word, Counted(-Net)), Mood::Bad);
    else Ctx.Say(std::format("{}: your chips come back, no more, no less.", Word));
}

void Roulette::PlaceChip(const Spot &On)
{
    if (Turning) {
        Ctx.Say("No more bets: the ball is running.");
        return;
    }
    const std::int64_t Chips = std::min(ChipNow(), Ctx.Chips.Now().Chips);
    if (Chips <= 0 || !Ctx.Chips.Take(Chips)) {
        Ctx.Say("You have no chips left to lay.", Mood::Bad);
        return;
    }
    Laid.Add(On, Chips);
    Ctx.Say(std::format("{}: {} chips on it, pays {} to 1.", Called(On), Counted(Laid.On(On)), Return(On) - 1));
}

void Roulette::TakeChip()
{
    if (Turning) return;
    const Spot         On    = Aim.Here().On;
    const std::int64_t Taken = Laid.Take(On, ChipNow());
    if (Taken == 0) return;
    Ctx.Chips.TakeBack(Taken);
    Ctx.Say(std::format("{}: {} chips taken back, {} left on it.", Called(On), Counted(Taken), Counted(Laid.On(On))));
}

// A split is laid on the cursor's number and its neighbour that way.
void Roulette::Split(int Across, int Down)
{
    const Place &From = Aim.Here();
    const Place *To   = PlaceAt(Aim.Across() + Across, Aim.Down() + Down);
    if (From.On.What != Kind::Straight || !To || To->On.What != Kind::Straight || To == &From) {
        Ctx.Say("There is no number that way to split with.");
        return;
    }
    const auto Low  = std::min(From.On.A, To->On.A);
    const auto High = std::max(From.On.A, To->On.A);
    PlaceChip(Spot{Kind::Split, Low, High});
}

void Roulette::Repeat()
{
    if (Turning || Before.Empty()) {
        Ctx.Say("There are no bets of the last round to repeat.");
        return;
    }
    if (!Ctx.Chips.Take(Before.Total())) {
        Ctx.Say("You have not enough chips to repeat the last bets.", Mood::Bad);
        return;
    }
    for (const Placed &One : Before.All()) Laid.Add(One.On, One.Chips);
    Ctx.Say(std::format("The last round's bets are laid again: {} chips.", Counted(Laid.Total())));
}

void Roulette::ClearBets()
{
    if (Turning || Laid.Empty()) return;
    Ctx.Chips.TakeBack(Laid.Total());
    Laid.Clear();
    Ctx.Say("The chips are taken back.");
}

void Roulette::ChangeChip(int By)
{
    Size = std::clamp(Size + By, 0, ChipSizes - 1);
    Ctx.Say(std::format("The chip in hand is worth {}.", Counted(ChipNow())));
}

// The arrow that names a split's neighbour, or any other key that gives it up.
bool Roulette::Aimed(const tui::KeyEvent &Key, std::uint64_t)
{
    Splitting = false;
    if (!Key.IsExtended()) {
        Ctx.Say("Split given up.");
        return true;
    }
    switch (Key.Scancode) {
    case tui::KeyCodes::Left:  Split(-1, 0); break;
    case tui::KeyCodes::Right: Split(1, 0); break;
    case tui::KeyCodes::Up:    Split(0, -1); break;
    case tui::KeyCodes::Down:  Split(0, 1); break;
    default:                   Ctx.Say("Split given up."); break;
    }
    return true;
}

bool Roulette::OnKey(const tui::KeyEvent &Key, std::uint64_t NowUs)
{
    const kb_event_t Raw{Key.Scancode, Key.Ascii, Key.Mods};
    if (KeyChordMatches(&Ctx.Shown.Keys[KeySpin], &Raw)) {
        Throw(NowUs);
        return true;
    }
    if (Splitting) return Aimed(Key, NowUs);
    if (Key.IsExtended()) {
        switch (Key.Scancode) {
        case tui::KeyCodes::Left:  Aim.Go(-1, 0); return true;
        case tui::KeyCodes::Right: Aim.Go(1, 0); return true;
        case tui::KeyCodes::Up:    Aim.Go(0, -1); return true;
        case tui::KeyCodes::Down:  Aim.Go(0, 1); return true;
        default:                   return false;
        }
    }
    if (!Plain(Key)) return false;
    switch (Key.Ascii) {
    case tui::KeyCodes::Enter:
    case '\r':                      PlaceChip(Aim.Here().On); return true;
    case tui::KeyCodes::Backspace:  TakeChip(); return true;
    case ' ':                       Throw(NowUs); return true;
    case '+':
    case '=':                       ChangeChip(1); return true;
    case '-':
    case '_':                       ChangeChip(-1); return true;
    case 'r':
    case 'R':                       Repeat(); return true;
    case 'x':
    case 'X':                       ClearBets(); return true;
    case 's':
    case 'S':
        if (Turning) return true;
        Splitting = true;
        Ctx.Say("Split: press the arrow toward the number to split with.");
        return true;
    default:                        return false;
    }
}

std::uint32_t Roulette::FrameMs(std::uint64_t NowUs) const
{
    if (Turning) return 33;
    return Blinking(NowUs) ? 175 : 0;
}

void Roulette::Leave()
{
    if (Turning && !Settled) Settle();
    if (Laid.Empty()) return;
    Ctx.Chips.TakeBack(Laid.Total());
    Laid.Clear();
}

std::vector<std::string> Roulette::Keys() const
{
    return {Entry("arrows", "move over the board"),
            Entry("enter", "lay a chip on the place under the cursor"),
            Entry("backspace", "take a chip back"),
            Entry(std::format("space, {}", Spelled(Ctx.Shown.Keys[KeySpin])), "spin the wheel; again, to hurry it"),
            Entry("+ and -", "the chip in hand: bigger, smaller"),
            Entry("s, then an arrow", "split: a chip between two numbers"),
            Entry("r", "lay the last round's bets again"),
            Entry("x", "take all the chips back")};
}

}  // namespace casino
