/* blackjack_paint.cpp — the blackjack table as it is drawn: where each card
 * stands, the cards in their slide and turn, the totals and outcomes under
 * the hands, the chips and history, and the hints (blackjack.h). */

#include <algorithm>
#include <cmath>
#include <format>

#include "blackjack.h"
#include "chips.h"
#include "common.h"
#include "ornament.h"
#include "painter/tui/glyph.h"
#include "painter/tui/text.h"
#include "trig.h"

namespace casino {

namespace {

namespace tui = painter::tui;

// How quickly a card follows the place it is to be at: the share of the way
// still to go falls to a third in about a twelfth of a second.
constexpr float Follow = 14.0f;

std::string TotalText(const Total &Held, std::size_t Cards, bool Natural)
{
    if (Cards == 0) return {};
    if (Held.Value > 21) return "BUST";
    if (Natural) return "BLACKJACK";
    return Held.Soft && Held.Value < 21 ? std::format("soft {}", Held.Value) : std::to_string(Held.Value);
}

std::string Signed(std::int64_t Net)
{
    return std::format("{}{}", Net > 0 ? "+" : "", Counted(Net));
}

}  // namespace

Blackjack::Layout Blackjack::Measure(const tui::Frame &Out) const
{
    Layout Where;
    Where.Cols       = Out.Size().Width;
    Where.Rows       = Out.Size().Height;
    Where.Card       = CardSizeFor(Where.Rows);
    Where.Step       = std::max(3, Where.Card.Cols * 58 / 100);
    const bool Roomy = Where.Rows >= 46;
    Where.TextRows   = Roomy ? 4 : 3;
    Where.LabelRows  = Roomy ? 2 : 1;
    Where.CircleRows = Where.Rows >= 34 ? 5 : 3;
    Where.RailWide   = Roomy ? 3.0f : 2.0f;
    Where.Bottom     = Where.Rows - 3;
    const std::int32_t Used = 2 * Where.Card.Rows + Where.TextRows + Where.LabelRows + Where.CircleRows;
    const std::int32_t Gap  = std::max(0, (Where.Bottom - Used) / 4);
    Where.DealerTop         = 1 + Gap;
    Where.TextTop           = Where.DealerTop + Where.Card.Rows + Gap;
    Where.PlayerTop         = Where.TextTop + Where.TextRows + Gap;
    Where.LabelRow          = Where.PlayerTop + Where.Card.Rows;
    Where.CircleTop         = Where.LabelRow + Where.LabelRows + Gap / 2;
    Where.ArcMiddle         = (static_cast<float>(Where.PlayerTop) + static_cast<float>(Where.Card.Rows) / 2.0f) * 2.0f;
    Where.ArcAcross         = static_cast<float>(Where.Cols) / 2.0f - 0.5f;
    Where.ArcDepth          = static_cast<float>(2 * (Where.Bottom + 1)) - Where.ArcMiddle;
    Where.ShoeWide          = Where.Card.Cols + 6;
    Where.ShoeRows          = Where.Card.Rows / 2 + 2;
    Where.ShoeLeft          = Where.Cols - Where.ShoeWide - 4;
    Where.TrayLeft          = 4;
    Where.ShoeAcross        = static_cast<float>(Where.ShoeLeft + 1);
    Where.ShoeDown          = static_cast<float>(2 * Where.DealerTop + 2);
    return Where;
}

// Where each card of the round that has left the shoe stands: the dealer's in
// a fan in the middle, each hand's in a fan in its share of the table.
std::vector<Blackjack::Spot> Blackjack::Places(const Layout &Where, std::uint64_t NowUs) const
{
    std::vector<Spot> Spots;
    const auto        Born = [&](const Dealt &One) { return One.Serial < Sprites.size() && NowUs >= Sprites[One.Serial].BornUs; };
    const auto        Fan  = [&](const std::vector<Dealt> &Cards, std::int32_t Hand, std::int32_t Row, std::int32_t Left, std::int32_t Room) {
        std::int32_t Count = 0;
        for (const Dealt &One : Cards) Count += Born(One) ? 1 : 0;
        const std::int32_t Step = Count > 1 ? std::clamp((Room - Where.Card.Cols) / (Count - 1), 3, Where.Step) : Where.Step;
        const std::int32_t Wide = Where.Card.Cols + std::max(0, Count - 1) * Step;
        std::int32_t       At   = 0;
        for (const Dealt &One : Cards) {
            if (!Born(One)) continue;
            Spots.push_back(Spot{One, Hand, {Left + (Room - Wide) / 2 + At * Step, Row}});
            At++;
        }
    };
    Fan(Game.Dealer().Cards, -1, Where.DealerTop, Where.Cols / 2 - Where.Cols / 4, Where.Cols / 2);
    const float        Reach  = static_cast<float>(2 * (Where.PlayerTop + Where.Card.Rows)) - Where.ArcMiddle;
    const float        Across = Where.ArcAcross * std::sqrt(std::max(0.0f, 1.0f - (Reach / Where.ArcDepth) * (Reach / Where.ArcDepth)));
    const std::int32_t Usable = std::max(Where.Card.Cols, static_cast<std::int32_t>(2.0f * (Across - Where.RailWide - 1.0f)));
    const auto         Count  = static_cast<std::int32_t>(Game.Hands().size());
    const std::int32_t Slot   = Usable / std::max(1, Count);
    for (std::int32_t Hand = 0; Hand < Count; Hand++)
        Fan(Game.Hands()[static_cast<std::size_t>(Hand)].Cards, Hand, Where.PlayerTop, Where.Cols / 2 - Usable / 2 + Hand * Slot, Slot);
    std::sort(Spots.begin(), Spots.end(), [](const Spot &A, const Spot &B) { return A.One.Serial < B.One.Serial; });
    return Spots;
}

// Each card a step nearer its place; at rest it stands on its cells, in
// motion it is drawn to the pixel, and a card turning over is squeezed to its
// edge and out again.
void Blackjack::PaintCards(tui::Frame &Out, const Layout &Where, std::uint64_t NowUs)
{
    const float Passed = PaintedUs == 0 ? 0.033f : std::min(static_cast<float>(NowUs - PaintedUs) / 1e6f, 0.1f);
    PaintedUs          = NowUs;
    const float Glide  = 1.0f - std::exp(-Follow * Passed * static_cast<float>(Ctx.Shown.Speed) / 100.0f);
    const float Turn   = Scaled(TurnUs);
    Moving             = false;
    for (const Spot &One : Places(Where, NowUs)) {
        Sprite     &Seen   = Sprites[One.One.Serial];
        const float Across = static_cast<float>(One.At.X), Down = static_cast<float>(One.At.Y * 2);
        if (!Seen.Placed) {
            Seen.Across = Where.ShoeAcross;
            Seen.Down   = Where.ShoeDown;
            Seen.Placed = true;
        }
        const float Share = Hurrying ? 1.0f : Glide;
        Seen.Across += (Across - Seen.Across) * Share;
        Seen.Down += (Down - Seen.Down) * Share;
        const bool Arrived = std::fabs(Across - Seen.Across) < 0.4f && std::fabs(Down - Seen.Down) < 0.4f;
        if (Arrived) {
            Seen.Across = Across;
            Seen.Down   = Down;
        } else {
            Moving = true;
        }
        const bool          Hole   = One.One.Serial == HoleSerial;
        const std::uint64_t Turned = Hole ? HoleFlipUs : Seen.FlipUs;
        const bool          Turns  = Hole ? HoleBooked : Seen.Flips;
        const float         Phase  = Turns && NowUs >= Turned ? std::min(static_cast<float>(NowUs - Turned) / Turn, 1.0f) : 0.0f;
        const bool          Up     = Turns && Phase >= 0.5f;
        const Card         *Face   = Up ? &One.One.Face : nullptr;
        if (Arrived && (!Turns || Phase == 0.0f || Phase >= 1.0f)) {
            PaintCard(Out, Ctx.Shown, Phase >= 1.0f ? &One.One.Face : nullptr, One.At, Where.Card);
            continue;
        }
        const float Squeeze = Turns && Phase > 0.0f && Phase < 1.0f ? std::fabs(Cosine(Pi * Phase)) : 1.0f;
        PaintMoving(Out, Ctx.Shown, Face, Seen.Across, Seen.Down, Where.Card, Squeeze);
    }
    Hurrying = false;
}

// What a settled hand says under itself, and the ink of it.
void Blackjack::PaintLabels(tui::Frame &Out, const Layout &Where, std::uint64_t NowUs)
{
    const Look &Shown = Ctx.Shown;
    struct Reach {
        std::int32_t       Left  = 1 << 20;
        std::int32_t       Right = -1;
        std::vector<Dealt> Seen;
    };
    Reach              Dealers;
    std::vector<Reach> Hands(Game.Hands().size());
    for (const Spot &One : Places(Where, NowUs)) {
        Reach &Of = One.Hand < 0 ? Dealers : Hands[static_cast<std::size_t>(One.Hand)];
        Of.Left   = std::min(Of.Left, One.At.X);
        Of.Right  = std::max(Of.Right, One.At.X + Where.Card.Cols);
        if (Landed(One.One, NowUs)) Of.Seen.push_back(One.One);
    }
    if (Dealers.Right >= 0) {
        const std::string Says = TotalText(TotalOf(Dealers.Seen), Dealers.Seen.size(), Dealers.Seen.size() == 2 && TotalOf(Dealers.Seen).Value == 21);
        const std::int32_t Row  = Where.DealerTop + Where.Card.Rows / 2 - 1;
        const std::int32_t Left = Dealers.Left - 2 - std::max<std::int32_t>(6, static_cast<std::int32_t>(Says.size()));
        Print(Out, {Left, Row}, "DEALER", Shown.Gold);
        const Color Ink = Says == "BUST" ? Shown.Lost : Says == "BLACKJACK" ? Shown.Gold : Shown.Text;
        Print(Out, {Left, Row + 1}, Says, Ink);
    }
    const std::int32_t Seats = static_cast<std::int32_t>(Hands.size());
    for (std::int32_t At = 0; At < Seats; At++) {
        const Reach &Of   = Hands[static_cast<std::size_t>(At)];
        const Hand  &Held = Game.Hands()[static_cast<std::size_t>(At)];
        if (Of.Right < 0) continue;
        const std::string Says  = TotalText(TotalOf(Of.Seen), Of.Seen.size(), Held.Natural() && Of.Seen.size() == 2);
        const bool        Turn  = Playing && Game.Now() == Phase::Player && Game.Active() == static_cast<std::size_t>(At);
        std::string       Under = std::format("bet {}", Counted(Held.Bet));
        Color             Ink   = Shown.Quiet;
        if (!Playing && Held.Result != Outcome::Open) {
            const std::int64_t Net = Held.Returned - Held.Bet;
            switch (Held.Result) {
            case Outcome::Blackjack: Under = std::format("BLACKJACK {}", Signed(Net)); Ink = Shown.Won; break;
            case Outcome::Win:       Under = std::format("WIN {}", Signed(Net)); Ink = Shown.Won; break;
            case Outcome::Push:      Under = "PUSH"; Ink = Shown.Text; break;
            case Outcome::Surrender: Under = std::format("GAVE UP {}", Signed(Net)); Ink = Shown.Quiet; break;
            case Outcome::Bust:      Under = std::format("BUST {}", Signed(Net)); Ink = Shown.Lost; break;
            default:                 Under = std::format("LOSE {}", Signed(Net)); Ink = Shown.Lost; break;
            }
        }
        const Color TotalInk = Says == "BUST" ? Shown.Lost : Says == "BLACKJACK" ? Shown.Gold : Turn ? Shown.Gold : Shown.Text;
        const std::string Mark = Turn && Seats > 1 ? std::string("\u25BA ") : std::string();
        const auto Centered = [&](std::int32_t Row, const std::string &Words, Color With) {
            const auto Wide = static_cast<std::int32_t>(tui::CellsOf(Words));
            Print(Out, {(Of.Left + Of.Right) / 2 - Wide / 2, Row}, Words, With);
        };
        const bool Settled = !Playing && Held.Result != Outcome::Open;
        const bool Alone   = Says.empty() || (Settled && (Says == "BUST" || Says == "BLACKJACK"));
        if (Where.LabelRows >= 2) {
            if (!Alone) Centered(Where.LabelRow, Mark + Says, TotalInk);
            Centered(Where.LabelRow + 1, Under, Ink);
        } else {
            const std::string Both = Alone ? Under : Mark + Says + "  " + (Settled ? Under : std::format("bet {}", Counted(Held.Bet)));
            Centered(Where.LabelRow, Both, Settled ? Ink : TotalInk);
        }
    }
}

void Blackjack::PaintInfo(tui::Frame &Out, const Layout &Where)
{
    const Look &Shown = Ctx.Shown;
    // Half the table's width on a row, inside the rail: the lines follow the arc.
    const auto Chord = [&](std::int32_t Down) {
        const float Deep = (static_cast<float>(2 * Down + 1) - Where.ArcMiddle) / Where.ArcDepth;
        return static_cast<std::int32_t>(Where.ArcAcross * std::sqrt(std::max(0.0f, 1.0f - Deep * Deep)) - Where.RailWide) - 2;
    };
    std::int64_t       Bets  = 0;
    for (const Hand &One : Game.Hands()) Bets += One.Bet;
    const std::int64_t Net = Ctx.Chips.Net();
    const std::int32_t Row = Where.CircleTop;
    const auto         Line = [&](std::int32_t At, std::string_view Name, const std::string &Value, Color Ink) {
        const std::int32_t Left = Where.Cols / 2 - Chord(Row + At);
        Print(Out, {Left, Row + At}, Name, Shown.Quiet);
        Print(Out, {Left + 7, Row + At}, Value, Ink);
    };
    Line(0, "chips", Counted(Ctx.Chips.Now().Chips), Shown.Text);
    Line(1, "bet", Counted(Playing ? Bets : Afford()), Shown.Text);
    Line(2, "visit", Signed(Net), Net >= 0 ? Shown.Won : Shown.Lost);
    if (History.empty()) return;
    std::vector<std::string> Words;
    std::int32_t             Wide = 0;
    const std::int32_t       Room = Chord(Row + 1) - 7;
    for (std::size_t At = History.size(); At-- > 0;) {
        const std::string One = History[At].Net == 0 ? "=" : Signed(History[At].Net);
        if (Wide + static_cast<std::int32_t>(One.size()) + 1 > Room) break;
        Words.insert(Words.begin(), One);
        Wide += static_cast<std::int32_t>(One.size()) + 1;
    }
    std::int32_t Across = Where.Cols / 2 + Chord(Row + 1) - Wide + 1;
    Print(Out, {Where.Cols / 2 + Chord(Row) - 10, Row}, "last rounds", Shown.Quiet);
    const std::size_t First = History.size() - Words.size();
    for (std::size_t At = 0; At < Words.size(); At++) {
        const Past &One = History[First + At];
        Print(Out, {Across, Row + 1}, Words[At], One.Natural ? Shown.Gold : One.Net > 0 ? Shown.Won : One.Net < 0 ? Shown.Lost : Shown.Text);
        Across += static_cast<std::int32_t>(Words[At].size()) + 1;
    }
}

void Blackjack::PaintHints(tui::Frame &Out, std::uint64_t NowUs) const
{
    std::vector<Hint> Hints;
    if (!Playing) {
        Hints.push_back({"space", "deal"});
        Hints.push_back({"+/-", std::format("bet {}", Counted(BetNow()))});
    } else if (!Calm(NowUs)) {
        Hints.push_back({"space", "show the cards"});
    } else if (Game.Now() == Phase::Insurance) {
        Hints.push_back({"i", std::format("insure {}", Counted(Game.Hands()[0].Bet / 2))});
        Hints.push_back({"n", "no insurance"});
    } else if (Game.Now() == Phase::Player) {
        const std::int64_t Extra = Game.Hands()[Game.Active()].Bet;
        Hints.push_back({"h", "hit"});
        Hints.push_back({"s", "stand"});
        if (Game.CanDouble()) Hints.push_back({"d", std::format("double {}", Counted(Extra))});
        if (Game.CanSplit()) Hints.push_back({"p", std::format("split {}", Counted(Extra))});
        if (Game.CanSurrender()) Hints.push_back({"r", "surrender"});
    }
    HintBar(Out, Ctx.Shown, Hints);
}

void Blackjack::Paint(tui::Frame &Out, std::uint64_t NowUs)
{
    const Layout Where = Measure(Out);
    PaintFelt(Out, Where);
    PaintPrinting(Out, Where);
    PaintShoe(Out, Where);
    PaintTray(Out, Where);
    PaintCircle(Out, Where);
    PaintCards(Out, Where, NowUs);
    PaintLabels(Out, Where, NowUs);
    PaintInfo(Out, Where);
    PaintedCalm = Calm(NowUs);
    Weave(Out, painter::core::Rect{0, 1, Where.Cols, Where.Bottom});
    PaintHints(Out, NowUs);
}

void Blackjack::PaintPay(tui::Frame &Out)
{
    const Look          &Shown = Ctx.Shown;
    const BlackjackRules &Rule = Shown.Blackjack;
    const Color          Back  = Color::Rgb(0x10, 0x10, 0x10);
    const tui::TextStyle Said{Shown.Text, Back};
    const tui::TextStyle Quiet{Shown.Quiet, Back};
    const std::int32_t   Wide = 62, High = 17;
    const painter::core::Rect Area{(Out.Size().Width - Wide) / 2, std::max(1, (Out.Size().Height - High) / 2), Wide, High};
    Box(Out, Area, Shown.Gold, Back, "Blackjack pays", Shown.Gold);
    const char *const Pays[][2] = {{"blackjack: an ace and a ten-card", "3 to 2"},
                                   {"any other winning hand", "1 to 1"},
                                   {"a push: the same total", "bet returned"},
                                   {"insurance, against a dealer blackjack", "2 to 1"}};
    for (std::int32_t At = 0; At < 4; At++) {
        Out.Write({Area.X + 3, Area.Y + 2 + At}, Pays[At][0], Said);
        Out.Write({Area.X + Wide - 3 - static_cast<std::int32_t>(std::string_view(Pays[At][1]).size()), Area.Y + 2 + At}, Pays[At][1],
                  tui::TextStyle{Shown.Gold, Back});
    }
    BoxRule(Out, Area, Area.Y + 7, Shown.Gold, Back);
    Out.Write({Area.X + 3, Area.Y + 8}, "The rules at this table (settings casino):", Quiet);
    const std::string Lines[] = {
        std::format("{} {}, dealer {} on a soft 17.", Rule.Decks, Rule.Decks == 1 ? "deck" : "decks", Rule.HitSoft17 ? "hits" : "stands"),
        std::format("Double {}{}.", Rule.DoubleAny ? "on any two cards" : "on a hard 9, 10 or 11", Rule.DoubleAfterSplit ? ", also after a split" : ""),
        std::format("Split up to {} hands; split aces take one card{}.", Rule.MaxHands, Rule.ResplitAces ? " and may be split again" : ""),
        std::format("Surrender is {}; insurance is {}.", Rule.Surrender ? "allowed" : "not allowed", Rule.Insurance ? "offered" : "not offered")};
    for (std::int32_t At = 0; At < 4; At++) Out.Write({Area.X + 3, Area.Y + 9 + At}, Lines[At], Said);
    Out.Write({Area.X + 3, Area.Y + 14}, "With the best play the house keeps about 0.4 to 0.7 chips in 100.", Quiet);
}

}  // namespace casino
