/* ledger.h — what the person has done at hangman: games, wins, the streak, the
 * best streak and the score, kept in a file of the volume (hangman.ledger) and
 * found again at the next start. */
#ifndef HANGMAN_LEDGER_H
#define HANGMAN_LEDGER_H

#include <cstdint>
#include <string>

namespace hangman {

struct Tally {
    std::uint32_t Games = 0;
    std::uint32_t Wins = 0;
    std::uint32_t Streak = 0;
    std::uint32_t Best = 0;
    std::uint64_t Score = 0;
};

class Ledger {
public:
    Ledger();

    const Tally &Totals() const { return Kept; }
    // A round against the computer ended, won or lost, and earned Points; the file is written.
    void Add(bool Won, std::uint32_t Points);
    // Why the file could not be read or written, else empty.
    const std::string &Trouble() const { return Said; }

private:
    void Save();

    Tally       Kept;
    std::string Said;
    bool        Writable = true;
};

}  // namespace hangman

#endif
