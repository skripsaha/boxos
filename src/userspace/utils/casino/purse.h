/* purse.h — the player's chips and what they came to, kept in casino.purse on
 * the volume (keep.h) and read again at the next start. A chip is a play
 * chip: nothing here is, or is ever shown as, money. */
#ifndef CASINO_PURSE_H
#define CASINO_PURSE_H

#include <cstdint>
#include <optional>
#include <string>

#include "look.h"

namespace casino {

struct Standing {
    std::int64_t Chips     = 0;
    std::int64_t Biggest   = 0;    // the most one round gave back above its stake
    std::int64_t Jackpots  = 0;
    std::int64_t Rounds    = 0;
    std::int64_t Wins      = 0;    // rounds that gave back more than they took
};

class Purse {
public:
    // The file read, or a new player's stack where there is none.
    explicit Purse(const Look &Shown);

    const Standing &Now() const { return Kept; }
    // Chips now against the visit's start, counting what lies on the table.
    std::int64_t Net() const { return Kept.Chips + OnTable - Opening; }
    std::int64_t OnTheTable() const { return OnTable; }

    // A stake laid on the table, or taken back from it. Take is false when
    // the chips are not there.
    bool Take(std::int64_t Chips);
    void TakeBack(std::int64_t Chips);
    // A round that took Staked and gave back Returned; the file is written.
    void Settle(std::int64_t Staked, std::int64_t Returned, bool Jackpot);

    // A fresh stack for a player with less than a chip and nothing on the
    // table, when the person allows it: the words for the glass.
    std::optional<std::string> Refilled(const Look &Shown);
    // The file written — unless it could not be read at the start: a record
    // that could not be read is never written over. Trouble says why not.
    void Save();

    const std::string &Trouble() const { return Said; }

private:
    Standing     Kept;
    std::int64_t Opening = 0;
    std::int64_t OnTable = 0;
    std::string  Said;
    bool         Writable = true;
};

}  // namespace casino

#endif
