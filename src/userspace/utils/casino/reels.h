/* reels.h — the slot machine's reels and what a line of them pays.
 *
 * All three reels are the same strip of 32 stops, and each stops at a place
 * drawn evenly from the 32: every combination of the three has the same
 * chance, 1 in 32768. The pay table is applied to the three symbols of each
 * line the player plays, a line paying a multiple of the bet on it; the
 * return to the player over time is worked out from the strip and the table
 * themselves (ReturnToPlayer), so the figure shown is the one the machine
 * really has. Three sevens on a line is the jackpot. */
#ifndef CASINO_REELS_H
#define CASINO_REELS_H

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace casino {

enum class Symbol : std::uint8_t { Seven, Bar, Bell, Star, Cherry, Lemon };

inline constexpr int SymbolKinds = 6;
inline constexpr int StripLength = 32;
inline constexpr int ReelCount   = 3;
inline constexpr int WindowRows  = 3;
inline constexpr int MostLines   = 5;

// The symbol at a stop of the strip; the strip is a ring.
Symbol           OnStrip(int Stop);
std::string_view NameOf(Symbol Which);

// The three symbols in view, by reel and row from the top.
struct Window {
    Symbol Seen[ReelCount][WindowRows];
};

// The window with Stops[reel] on each reel's middle row.
Window WindowAt(std::span<const int, ReelCount> Stops);

// The row of each reel a line crosses: the middle, the top, the bottom, and
// the two diagonals. The first one, three, or five are played.
struct Line {
    int Row[ReelCount];
};
const Line &LineAt(int Which);

// One row of the pay table: what must show on a line, and the multiple of the
// bet on the line it pays. A reel left unnamed (Any) may show anything.
struct Pay {
    std::string_view Words;
    Symbol           Needs[ReelCount];
    bool             Any[ReelCount];
    int              Multiple;
};
std::span<const Pay> PayTable();

// A line that paid.
struct Win {
    int  Line = 0;
    int  Multiple = 0;
    bool Jackpot = false;
    int  Reels = 0;       // how many reels, from the left, make the win
};

// The lines of the first Lines that pay in this window.
std::vector<Win> Evaluate(const Window &Seen, int Lines);

// Of every chip bet on a line, what comes back over all the combinations.
double ReturnToPlayer();
// How many in 32768 combinations pay on a line, and how many are the jackpot.
int PayingCombinations();
int JackpotCombinations();

}  // namespace casino

#endif
