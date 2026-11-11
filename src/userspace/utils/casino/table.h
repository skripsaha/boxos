/* table.h — what the roulette table and the slot machine have in common: they
 * are shown on the glass under the casino's header, take keys, say one line to
 * the player, and settle with the same purse. */
#ifndef CASINO_TABLE_H
#define CASINO_TABLE_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "look.h"
#include "luck.h"
#include "painter/tui/frame.h"
#include "painter/tui/stage.h"
#include "purse.h"

namespace casino {

// How the line said to the player is to be inked.
enum class Mood : std::uint8_t { Plain, Good, Bad };

// What both tables share: the settings as last read, the chips, the chances,
// and the line said to the player.
struct Context {
    Look         Shown;
    std::uint32_t Revision = 0;      // counts the times the settings were read again
    Purse        Chips;
    Luck         Chance;
    std::string  Notice;
    Mood         Feeling = Mood::Plain;

    explicit Context(const Look &First) : Shown(First), Chips(First) {}

    void Say(std::string Words, Mood How = Mood::Plain)
    {
        Notice  = std::move(Words);
        Feeling = How;
    }
};

class Table {
public:
    virtual ~Table() = default;

    // Time moves; a round that reached its end is settled.
    virtual void Advance(std::uint64_t NowUs) = 0;
    // The table drawn into Out, from the row under the header to the line of
    // hints, which is the last.
    virtual void Paint(painter::tui::Frame &Out, std::uint64_t NowUs) = 0;
    virtual void PaintPay(painter::tui::Frame &Out) = 0;
    // True when the key was the table's.
    virtual bool OnKey(const painter::tui::KeyEvent &Key, std::uint64_t NowUs) = 0;
    // How long until the picture must be drawn again by itself, 0 when it is
    // still: then it is drawn again only for a key or a changed setting.
    virtual std::uint32_t FrameMs(std::uint64_t NowUs) const = 0;
    // A round in play: the table is not left in the middle of it.
    virtual bool Busy() const = 0;
    // The player goes; whatever lies unplayed on the table is theirs again.
    virtual void Leave() = 0;
    virtual std::vector<std::string> Keys() const = 0;
};

}  // namespace casino

#endif
