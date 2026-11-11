/* slots.h — the slot machine: three reels behind a window of three rows, a
 * lever, the lines played, and what they paid.
 *
 * A pull takes the stake and draws where each reel is to stop, fairly (reels.h).
 * The motion is then worked out backwards from the stops: a reel runs at full
 * speed, brakes, and settles onto its stop with a small bounce, and the next
 * reel stops a little after it. The pay is settled once the last reel is
 * still. */
#ifndef CASINO_SLOTS_H
#define CASINO_SLOTS_H

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "canvas.h"
#include "faces.h"
#include "reels.h"
#include "shower.h"
#include "table.h"

namespace casino {

class Slots : public Table {
public:
    explicit Slots(Context &Shared);

    void Advance(std::uint64_t NowUs) override;
    void Paint(painter::tui::Frame &Out, std::uint64_t NowUs) override;
    void PaintPay(painter::tui::Frame &Out) override;
    bool OnKey(const painter::tui::KeyEvent &Key, std::uint64_t NowUs) override;
    std::uint32_t FrameMs(std::uint64_t NowUs) const override;
    bool Busy() const override { return Running; }
    void Leave() override;
    std::vector<std::string> Keys() const override;

private:
    // One reel's motion, worked out at the pull: where it started, how fast it
    // runs, when it brakes, and the place on the strip it comes to rest on.
    struct Drum {
        float Start  = 0;
        float BrakeAt = 0;
        float Target = 0;
        int   Stop   = 0;
    };

    // The glass divided: the cabinet and its parts, in pixels, and the panel
    // beside it, in cells.
    struct Layout {
        std::int32_t Edge       = 8;      // a symbol is Edge pixels square
        std::int32_t CabLeft    = 0;
        std::int32_t CabTop     = 0;
        std::int32_t CabWide    = 0;
        std::int32_t CabHigh    = 0;
        std::int32_t MarqueeHigh = 0;
        std::int32_t WindowTop  = 0;
        std::int32_t Letters    = 1;      // the marquee's letter size
        std::int32_t PanelLeft  = 0;      // in cells
        std::int32_t PanelWide  = 0;
        std::int32_t Cols       = 0;
        std::int32_t Rows       = 0;
    };

    // slots.cpp: the rules and the keys.
    void  Pull(std::uint64_t NowUs);
    void  Plan(int Reel, float Earliest);
    void  Settle(std::uint64_t NowUs);
    void  ChangeBet(int By);
    void  ChangeLines();
    float Seconds(std::uint64_t NowUs) const;
    float Position(int Reel, float Time) const;
    float Pace(int Reel, float Time) const;
    float EndsAt() const;
    std::int64_t Bet() const;
    std::int64_t Stake() const { return Bet() * Lines; }
    bool  Celebrating(std::uint64_t NowUs) const;

    // slots_paint.cpp: the picture.
    Layout Measure(const painter::tui::Frame &Out);
    void   PaintCabinet(Canvas &Draw, const Layout &Where, std::uint64_t NowUs);
    void   PaintLights(Canvas &Draw, const Layout &Where, std::uint64_t NowUs);
    void   PaintMarquee(Canvas &Draw, const Layout &Where, std::uint64_t NowUs);
    void   PaintReels(Canvas &Draw, const Layout &Where, std::uint64_t NowUs);
    void   PaintReel(Canvas &Draw, const Layout &Where, int Reel, std::int32_t Left, float Time);
    void   PaintWins(Canvas &Draw, const Layout &Where, std::uint64_t NowUs);
    void   PaintLever(Canvas &Draw, const Layout &Where, std::uint64_t NowUs);
    void   PaintPanel(painter::tui::Frame &Out, const Layout &Where);

    Context                     &Ctx;
    std::array<Drum, ReelCount>  Drums;
    std::array<int, ReelCount>   Rests{};          // the stop at rest on each reel's middle row
    std::vector<Win>             Wins;
    std::optional<Faces>         Art;
    Shower                       Coins;
    bool                         Running  = false;
    bool                         Jackpot  = false;
    std::uint64_t                PulledUs = 0;
    std::uint64_t                SettledUs = 0;
    std::uint64_t                SteppedUs = 0;
    std::int64_t                 Paid     = 0;
    std::int64_t                 Staked   = 0;
    int                          Size     = 0;
    int                          Lines    = 3;
    std::int32_t                 GlassWide = 80;       // the glass as last painted, in pixels
    std::int32_t                 GlassHigh = 50;
};

}  // namespace casino

#endif
