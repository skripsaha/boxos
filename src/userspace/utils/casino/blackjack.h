/* blackjack.h — the blackjack table: the rules (blackjack_game.h) with a felt
 * table, a shoe, a discard tray, a betting circle and cards that slide from
 * the shoe and turn over.
 *
 * The game counts at once; the table shows it at the pace of the dealer's
 * hand. Every card has its place in the picture, drawn from the shoe to it
 * and turned over, and the round's chips are settled only when the last card
 * has landed and the hole card has been shown. Keys wait for the cards. */
#ifndef CASINO_BLACKJACK_H
#define CASINO_BLACKJACK_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "blackjack_game.h"
#include "card_art.h"
#include "painter/core/geometry.h"
#include "table.h"

namespace casino {

class Blackjack : public Table {
public:
    // The pace of a round at normal speed, in microseconds: a card leaves the
    // shoe this long after the one before, takes this long to slide and this
    // long to turn over; the round is settled this long after the last card
    // is seen. The dealer's second card is the fourth drawn.
    static constexpr std::uint64_t GapUs      = 330000;
    static constexpr std::uint64_t SlideUs    = 300000;
    static constexpr std::uint64_t TurnUs     = 240000;
    static constexpr std::uint64_t ReadUs     = 500000;
    static constexpr std::uint32_t HoleSerial = 3;

    explicit Blackjack(Context &Shared) : Ctx(Shared), Game(Shared.Chance) {}

    void Advance(std::uint64_t NowUs) override;
    void Paint(painter::tui::Frame &Out, std::uint64_t NowUs) override;
    void PaintPay(painter::tui::Frame &Out) override;
    bool OnKey(const painter::tui::KeyEvent &Key, std::uint64_t NowUs) override;
    std::uint32_t FrameMs(std::uint64_t NowUs) const override;
    bool Busy() const override { return Playing; }
    void Leave() override;
    std::vector<std::string> Keys() const override;

private:
    // A card of the round in the picture: when it leaves the shoe, when it
    // turns over, and where it is on the glass now, in pixels.
    struct Sprite {
        std::uint64_t BornUs = 0;
        std::uint64_t FlipUs = 0;
        bool          Flips  = false;
        bool          Placed = false;
        float         Across = 0;
        float         Down   = 0;
    };

    // Where a card of the round stands: of the dealer's hand (-1) or a player's.
    struct Spot {
        Dealt                One;
        std::int32_t         Hand = -1;
        painter::core::Point At;
    };

    // A round gone by, for the short history.
    struct Past {
        std::int64_t Net     = 0;
        bool         Natural = false;
    };

    // The glass divided, in cells unless said to be pixels.
    struct Layout {
        std::int32_t Cols = 0;
        std::int32_t Rows = 0;
        CardSize     Card;
        std::int32_t Step         = 4;       // cells between the cards of a fan
        std::int32_t DealerTop    = 1;
        std::int32_t TextTop      = 0;
        std::int32_t TextRows     = 3;
        std::int32_t PlayerTop    = 0;
        std::int32_t LabelRow     = 0;
        std::int32_t LabelRows    = 1;
        std::int32_t CircleTop    = 0;
        std::int32_t CircleRows   = 3;
        std::int32_t Bottom       = 0;       // the last row of the table
        float        ArcMiddle    = 0;       // the arc's middle and its two reaches, in pixels
        float        ArcAcross    = 0;
        float        ArcDepth     = 0;
        float        RailWide     = 2;
        std::int32_t ShoeLeft     = 0;       // the shoe and the discard tray, in cells
        std::int32_t ShoeWide     = 0;
        std::int32_t ShoeRows     = 0;
        std::int32_t TrayLeft     = 0;
        float        ShoeAcross   = 0;       // where a card leaves the shoe, in pixels
        float        ShoeDown     = 0;
    };

    // blackjack.cpp: the rules, the keys and the pace of the round.
    void   Deal(std::uint64_t NowUs);
    bool   Spend(std::int64_t Chips, const char *For);
    void   Insure(bool Take);
    void   Double();
    void   Split();
    void   Surrender();
    void   ChangeBet(int By);
    void   Hurry();
    void   Sync(std::uint64_t NowUs);
    void   BookHoleFlip(std::uint64_t NowUs);
    void   Finish(std::uint64_t NowUs);
    bool   Act(char Pressed);
    void   Announce();
    bool   Calm(std::uint64_t NowUs) const;
    bool   Landed(const Dealt &One, std::uint64_t NowUs) const;
    std::int64_t BetNow() const;
    std::int64_t Afford() const;
    float  Scaled(std::uint64_t Us) const;

    // blackjack_paint.cpp: the picture.
    Layout Measure(const painter::tui::Frame &Out) const;
    void   PaintCards(painter::tui::Frame &Out, const Layout &Where, std::uint64_t NowUs);
    void   PaintLabels(painter::tui::Frame &Out, const Layout &Where, std::uint64_t NowUs);
    void   PaintInfo(painter::tui::Frame &Out, const Layout &Where);
    void   PaintHints(painter::tui::Frame &Out, std::uint64_t NowUs) const;
    std::vector<Spot> Places(const Layout &Where, std::uint64_t NowUs) const;

    // blackjack_table.cpp: the felt, its rail, the printing on it, the shoe,
    // the discard tray and the betting circle.
    void PaintFelt(painter::tui::Frame &Out, const Layout &Where) const;
    void PaintPrinting(painter::tui::Frame &Out, const Layout &Where) const;
    void PaintShoe(painter::tui::Frame &Out, const Layout &Where) const;
    void PaintTray(painter::tui::Frame &Out, const Layout &Where) const;
    void PaintCircle(painter::tui::Frame &Out, const Layout &Where) const;

    Context                &Ctx;
    BlackjackGame           Game;
    std::vector<Sprite>     Sprites;
    std::vector<Past>       History;
    std::uint32_t           Known       = 0;       // cards of the round the picture has been told of
    std::uint64_t           NextBornUs  = 0;       // when the next card may leave the shoe
    std::uint64_t           LandUs      = 0;       // when the last card seen is turned and still
    std::uint64_t           HoleFlipUs  = 0;
    std::uint64_t           SettledUs   = 0;
    std::uint64_t           PaintedUs   = 0;
    bool                    HoleBooked  = false;
    bool                    Playing     = false;   // a round has begun and is not yet settled
    bool                    Moving      = false;   // a card was still on its way when last painted
    bool                    Hurrying    = false;
    bool                    PaintedCalm = false;   // the cards were still when last painted
    int                     Size        = 0;       // the chip that sets the bet
    std::int64_t            Discarded   = 0;       // cards in the discard tray
};

}  // namespace casino

#endif
