/* blackjack.cpp — the blackjack table's keys and the pace of a round
 * (blackjack.h). */

#include "blackjack.h"

#include <algorithm>
#include <format>

#include "chips.h"
#include "common.h"

namespace casino {

namespace {

namespace tui = painter::tui;

constexpr std::size_t   MostPast   = 12;

bool Plain(const tui::KeyEvent &Key)
{
    return !Key.IsExtended() && !Key.IsCtrl() && !Key.IsAlt();
}

}  // namespace

float Blackjack::Scaled(std::uint64_t Us) const
{
    return static_cast<float>(Us) * 100.0f / static_cast<float>(Ctx.Shown.Speed);
}

std::int64_t Blackjack::BetNow() const
{
    return ChipValue(Ctx.Shown, Size);
}

// The bet as it will be laid: what the chip is worth, or all the player has.
std::int64_t Blackjack::Afford() const
{
    return std::min(BetNow(), Ctx.Chips.Now().Chips);
}

bool Blackjack::Spend(std::int64_t Chips, const char *For)
{
    if (Chips > 0 && Ctx.Chips.Take(Chips)) return true;
    Ctx.Say(std::format("There are not enough chips to {}.", For), Mood::Bad);
    return false;
}

void Blackjack::Deal(std::uint64_t NowUs)
{
    const std::int64_t Bet = Afford();
    if (Bet <= 0 || !Ctx.Chips.Take(Bet)) {
        Ctx.Say("You have no chips left to bet.", Mood::Bad);
        return;
    }
    Sprites.clear();
    Known      = 0;
    NextBornUs = NowUs;
    LandUs     = NowUs;
    HoleBooked = false;
    Hurrying   = false;
    Moving     = false;
    Playing    = true;
    Game.Begin(Bet, Ctx.Shown.Blackjack);
    if (Game.Reshuffled()) Discarded = 0;
    Sync(NowUs);
    Announce();
}

// The picture is told of each new card in the order the shoe gave it, and of
// the hole card's turning, which comes before the dealer's own draws.
void Blackjack::Sync(std::uint64_t NowUs)
{
    while (Known < Game.Drawn()) {
        const bool Draw = Known > HoleSerial;
        bool       Dealers = false;
        for (const Dealt &One : Game.Dealer().Cards) Dealers = Dealers || One.Serial == Known;
        if (Draw && Dealers && Game.HoleShown() && !HoleBooked) BookHoleFlip(NowUs);
        Sprite One;
        One.BornUs = std::max(NowUs, NextBornUs);
        NextBornUs = One.BornUs + static_cast<std::uint64_t>(Scaled(GapUs));
        One.Flips  = Known != HoleSerial;
        One.FlipUs = One.BornUs + static_cast<std::uint64_t>(Scaled(SlideUs));
        LandUs     = std::max(LandUs, One.FlipUs + static_cast<std::uint64_t>(Scaled(One.Flips ? TurnUs : 0)));
        Sprites.push_back(One);
        Known++;
    }
    if (Game.HoleShown() && !HoleBooked && Known == Game.Drawn()) BookHoleFlip(NowUs);
}

void Blackjack::BookHoleFlip(std::uint64_t NowUs)
{
    HoleFlipUs = std::max({NowUs, NextBornUs, LandUs});
    NextBornUs = HoleFlipUs + static_cast<std::uint64_t>(Scaled(TurnUs + GapUs / 2));
    LandUs     = HoleFlipUs + static_cast<std::uint64_t>(Scaled(TurnUs));
    HoleBooked = true;
}

bool Blackjack::Landed(const Dealt &One, std::uint64_t NowUs) const
{
    if (One.Serial >= Sprites.size()) return false;
    const Sprite &Seen = Sprites[One.Serial];
    if (NowUs < Seen.BornUs) return false;
    if (One.Serial == HoleSerial) return HoleBooked && NowUs >= HoleFlipUs + static_cast<std::uint64_t>(Scaled(TurnUs));
    return NowUs >= Seen.FlipUs + static_cast<std::uint64_t>(Scaled(TurnUs));
}

bool Blackjack::Calm(std::uint64_t NowUs) const
{
    return Known == Game.Drawn() && NowUs >= LandUs && !Moving && (!Game.HoleShown() || HoleBooked);
}

// Everything the round shows is shown at once.
void Blackjack::Hurry()
{
    for (Sprite &One : Sprites) {
        One.BornUs = 0;
        One.FlipUs = 0;
    }
    NextBornUs = 0;
    HoleFlipUs = 0;
    LandUs     = 0;
    Hurrying   = true;
}

void Blackjack::Advance(std::uint64_t NowUs)
{
    if (!Playing) return;
    Sync(NowUs);
    if (Game.Now() == Phase::Over && Calm(NowUs) && NowUs >= LandUs + static_cast<std::uint64_t>(Scaled(ReadUs))) Finish(NowUs);
}

void Blackjack::Finish(std::uint64_t NowUs)
{
    const std::int64_t Staked   = Game.Staked();
    const std::int64_t Returned = Game.Returned();
    const std::int64_t Net      = Returned - Staked;
    Ctx.Chips.Settle(Staked, Returned, false);
    bool Natural = false;
    for (const Hand &One : Game.Hands()) Natural = Natural || One.Result == Outcome::Blackjack;
    History.push_back(Past{Net, Natural});
    if (History.size() > MostPast) History.erase(History.begin());
    Discarded += Game.Drawn();
    Playing   = false;
    SettledUs = NowUs;
    Announce();
}

void Blackjack::Announce()
{
    const std::vector<Hand> &Hands = Game.Hands();
    switch (Game.Now()) {
    case Phase::Insurance:
        Ctx.Say(std::format("Insurance? It costs {} chips and pays 2 to 1.", Counted(Hands[0].Bet / 2)));
        return;
    case Phase::Player:
        Ctx.Say(Hands.size() > 1 ? std::format("Hand {} of {}: your move.", Game.Active() + 1, Hands.size()) : std::string("Your move."));
        return;
    case Phase::Over: break;
    case Phase::Betting: return;
    }
    if (Playing) {
        Ctx.Say("The dealer turns over the cards.");
        return;
    }
    const std::int64_t Net = Game.Returned() - Game.Staked();
    std::string        Said;
    if (Hands.size() == 1) {
        switch (Hands[0].Result) {
        case Outcome::Blackjack: Said = "Blackjack! Three to two."; break;
        case Outcome::Bust:      Said = "Bust."; break;
        case Outcome::Push:      Said = "Push."; break;
        case Outcome::Surrender: Said = "You give up the hand."; break;
        case Outcome::Win:       Said = Game.Dealer().Count().Value > 21 ? "The dealer busts." : "You win."; break;
        default:                 Said = Game.Dealer().Natural() ? "The dealer has blackjack." : "The dealer wins."; break;
        }
    } else {
        Said = std::format("{} hands played.", Hands.size());
    }
    if (Game.InsuredFor() > 0) Said += Game.Dealer().Natural() ? " The insurance pays." : " The insurance is lost.";
    if (Net > 0) Ctx.Say(std::format("{} You win {} chips.", Said, Counted(Net)), Mood::Good);
    else if (Net < 0) Ctx.Say(std::format("{} You lose {} chips.", Said, Counted(-Net)), Mood::Bad);
    else Ctx.Say(std::format("{} Your chips come back.", Said));
}

void Blackjack::Insure(bool Take)
{
    if (Take && !Spend(Game.Hands()[0].Bet / 2, "insure")) return;
    Game.Insure(Take);
    Announce();
}

void Blackjack::Double()
{
    if (!Game.CanDouble()) {
        Ctx.Say("This hand cannot be doubled.");
        return;
    }
    if (!Spend(Game.Hands()[Game.Active()].Bet, "double")) return;
    Game.Double();
    Announce();
}

void Blackjack::Split()
{
    if (!Game.CanSplit()) {
        Ctx.Say("This hand cannot be split.");
        return;
    }
    if (!Spend(Game.Hands()[Game.Active()].Bet, "split")) return;
    Game.Split();
    Announce();
}

void Blackjack::Surrender()
{
    if (!Game.CanSurrender()) {
        Ctx.Say("The hand cannot be given up now.");
        return;
    }
    Game.Surrender();
    Announce();
}

void Blackjack::ChangeBet(int By)
{
    if (Playing) return;
    Size = std::clamp(Size + By, 0, ChipSizes - 1);
    Ctx.Say(std::format("The bet is {} chips.", Counted(BetNow())));
}

bool Blackjack::OnKey(const tui::KeyEvent &Key, std::uint64_t NowUs)
{
    const kb_event_t Raw{Key.Scancode, Key.Ascii, Key.Mods};
    const bool       Spin = KeyChordMatches(&Ctx.Shown.Keys[KeySpin], &Raw);
    if (Key.IsExtended() && !Spin) {
        if (Key.Scancode == tui::KeyCodes::Up) ChangeBet(1);
        else if (Key.Scancode == tui::KeyCodes::Down) ChangeBet(-1);
        else return false;
        return true;
    }
    if (!Spin && !Plain(Key)) return false;
    char Pressed = Spin ? ' ' : Key.Ascii;
    if (Pressed >= 'A' && Pressed <= 'Z') Pressed = static_cast<char>(Pressed - 'A' + 'a');
    if (Pressed == '=') Pressed = '+';
    if (Pressed == '_') Pressed = '-';
    if (Pressed == tui::KeyCodes::Enter || Pressed == '\r') Pressed = ' ';
    if (Pressed == '+' || Pressed == '-') {
        ChangeBet(Pressed == '+' ? 1 : -1);
        return true;
    }
    if (!Playing) {
        if (Pressed != ' ') return false;
        Deal(NowUs);
        return true;
    }
    if (!Calm(NowUs)) {
        Hurry();
        return true;
    }
    const Phase Now = Game.Now();
    if (Now == Phase::Insurance && (Pressed == 'i' || Pressed == 'n')) Insure(Pressed == 'i');
    else if (Now != Phase::Player || !Act(Pressed)) return false;
    Sync(NowUs);
    return true;
}

// A key of the player's turn; false when it is none of them.
bool Blackjack::Act(char Pressed)
{
    switch (Pressed) {
    case 'h': Game.Hit(); Announce(); return true;
    case 's': Game.Stand(); Announce(); return true;
    case 'd': Double(); return true;
    case 'p': Split(); return true;
    case 'r': Surrender(); return true;
    default:  return false;
    }
}

std::uint32_t Blackjack::FrameMs(std::uint64_t NowUs) const
{
    if (Playing && (!Calm(NowUs) || !PaintedCalm || Game.Now() == Phase::Over)) return 33;
    return SettledUs != 0 && NowUs - SettledUs < 250000 ? 50 : 0;
}

// The player goes in the middle of a round: every open hand stands.
void Blackjack::Leave()
{
    if (!Playing) return;
    Game.Abandon();
    Finish(0);
}

std::vector<std::string> Blackjack::Keys() const
{
    return {Entry(std::format("space, {}", Spelled(Ctx.Shown.Keys[KeySpin])), "deal; during a round, show the cards at once"),
            Entry("+ and -, up, down", "the bet: bigger, smaller"),
            Entry("h", "hit: one more card"),
            Entry("s", "stand: keep the hand"),
            Entry("d", "double: double the bet, one card more"),
            Entry("p", "split a pair into two hands"),
            Entry("i, n", "insurance against a dealer's blackjack: yes, no"),
            Entry("r", "surrender: half the bet back, where it is allowed")};
}

}  // namespace casino
