/* desk.h — the casino floor: the glass, the header over the tables, the line
 * said to the player, the keys that are the casino's own (the other table,
 * the pay table, help, leaving), and the loop that moves time. */
#ifndef CASINO_DESK_H
#define CASINO_DESK_H

#include <cstdint>

#include "blackjack.h"
#include "box/cxx/settings.h"
#include "painter/tui/frame.h"
#include "painter/tui/stage.h"
#include "roulette.h"
#include "slots.h"
#include "table.h"

namespace casino {

enum class Sitting : std::uint8_t { Roulette, Slots, Blackjack };

inline constexpr int TableCount = 3;

class Desk {
public:
    Desk(painter::tui::Stage Opened, Sitting Start);

    // Until the player leaves, or the casino is asked to finish.
    void Run();
    // What the player leaves with.
    const Standing &Final() const { return Ctx.Chips.Now(); }
    std::int64_t    Net() const { return Ctx.Chips.Net(); }
    const std::string &Trouble() const { return Ctx.Chips.Trouble(); }

private:
    bool OnKey(const painter::tui::KeyEvent &Key, std::uint64_t NowUs);
    void Sit(int Which);
    void Refill();

    void Paint(std::uint64_t NowUs);
    void PaintHeader(painter::tui::Frame &Out) const;
    void PaintNotice(painter::tui::Frame &Out) const;
    void PaintHelp(painter::tui::Frame &Out) const;

    painter::tui::Stage  Screen;
    box::settings::watch Watch;
    Context              Ctx;
    Roulette             Wheel;
    Slots                Machine;
    Blackjack            Cards;
    Table               *Tables[TableCount];
    int                  Seat   = 0;
    bool                 Help   = false;
    bool                 Paying = false;
};

}  // namespace casino

#endif
