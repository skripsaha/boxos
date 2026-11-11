/* roulette.h — the roulette table: the wheel with its ball, the betting board
 * with its cursor, the chips laid on it, and the results so far.
 *
 * A round has three parts. Bets are laid while the wheel is still. A spin
 * draws the number, fairly, and sends the ball round (wheel.h); the bets are
 * settled the moment the ball lies in its pocket, while the wheel is still
 * slowing; the wheel is still when the round is over and the next may begin. */
#ifndef CASINO_ROULETTE_H
#define CASINO_ROULETTE_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "bets.h"
#include "board.h"
#include "canvas.h"
#include "table.h"
#include "wheel.h"
#include "wheel_plate.h"

namespace casino {

class Roulette : public Table {
public:
    explicit Roulette(Context &Shared) : Ctx(Shared) {}

    void Advance(std::uint64_t NowUs) override;
    void Paint(painter::tui::Frame &Out, std::uint64_t NowUs) override;
    void PaintPay(painter::tui::Frame &Out) override;
    bool OnKey(const painter::tui::KeyEvent &Key, std::uint64_t NowUs) override;
    std::uint32_t FrameMs(std::uint64_t NowUs) const override;
    bool Busy() const override { return Turning.has_value(); }
    void Leave() override;
    std::vector<std::string> Keys() const override;

private:
    // The glass divided: where the wheel stands and how big the board's
    // cells are, from the size of the frame.
    struct Layout {
        float        WheelAcross = 0;     // the wheel's center and radius, in pixels
        float        WheelDown   = 0;
        float        Radius      = 0;
        std::int32_t BoardLeft   = 0;     // the board's top left cell, in cells of text
        std::int32_t BoardTop    = 0;
        std::int32_t CellWide    = 3;
        std::int32_t CellHigh    = 2;
        std::int32_t PanelTop    = 0;     // under the board
        std::int32_t TrayTop     = 0;     // under the wheel: results and chips
        std::int32_t Cols        = 0;
        std::int32_t Rows        = 0;
    };

    // roulette.cpp: the rules and the keys.
    void Throw(std::uint64_t NowUs);
    void Settle();
    void PlaceChip(const Spot &On);
    void TakeChip();
    void Split(int Across, int Down);
    void Repeat();
    void ClearBets();
    void ChangeChip(int By);
    bool Aimed(const painter::tui::KeyEvent &Key, std::uint64_t NowUs);
    std::int64_t ChipNow() const;
    float        Seconds(std::uint64_t NowUs) const;

    // roulette_paint.cpp: the picture.
    Layout Measure(const painter::tui::Frame &Out) const;
    void   PaintWheel(Canvas &Draw, const Layout &Where, std::uint64_t NowUs);
    void   PaintBoard(painter::tui::Frame &Out, const Layout &Where, std::uint64_t NowUs);
    void   PaintCell(painter::tui::Frame &Out, const Layout &Where, const Place &One, std::uint64_t NowUs);
    void   PaintSplits(painter::tui::Frame &Out, const Layout &Where);
    void   PaintPanel(painter::tui::Frame &Out, const Layout &Where);
    void   PaintTray(painter::tui::Frame &Out, const Layout &Where);
    void   PaintHints(painter::tui::Frame &Out, const Layout &Where);
    bool   Lit(std::uint64_t NowUs) const;
    bool   Blinking(std::uint64_t NowUs) const;

    Context            &Ctx;
    Bets                Laid;
    Bets                Before;               // the last round's, for r
    Cursor              Aim;
    std::vector<int>    Results;              // newest last
    std::optional<Spin> Turning;              // the round in play, until the wheel is still
    std::uint64_t       ThrownUs = 0;
    bool                Settled  = false;     // the round's bets are paid
    int                 Number   = -1;        // the last number, -1 before any
    int                 Size     = 0;         // the chip in hand: index into the sizes
    bool                Splitting = false;    // the next arrow lays a split
    Flight              Last;                 // what the wheel and ball were last drawn at
    std::uint64_t       EndedUs  = 0;         // when the wheel last came to rest
    WheelPlate          Plate;
};

}  // namespace casino

#endif
