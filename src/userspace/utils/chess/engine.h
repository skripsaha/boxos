/* engine.h — the computer's play: how good a position is, and the move it
 * finds best in the time it is given.
 *
 * A position is weighed by its material and where each piece stands — a
 * middle game's table and an end game's, blended by how much is left — with
 * the bishop pair, pawns doubled, isolated and passed, rooks on open files
 * and the pawns before a castled king. The search deepens a ply at a time
 * until its time is up: alpha-beta with a principal variation, captures
 * followed until the position is quiet, positions remembered by their key,
 * the best move tried first, captures by what they win, quiet moves that cut
 * before; a null move and late moves searched shallower where that is safe.
 *
 * A level is how deep and how long it looks and how much it lets its own
 * whim stir its judgement: at the bottom a beginner's game, at the top as
 * much as the machine finds in a few seconds. */
#ifndef CHESS_ENGINE_H
#define CHESS_ENGINE_H

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "position.h"

namespace chess {

inline constexpr int Levels = 6;

struct Limits {
    int           Depth  = 64;   // plies at most
    std::uint64_t DueUs  = 0;    // the moment it must answer by, of clock_uptime_us
    int           Whim   = 0;    // a position's worth stirred by up to this, in hundredths of a pawn
    std::uint64_t Seed   = 0;    // the game's own whim: the same position, another game, another stir
};

/* The limits of a level, 1 to Levels, from Now. */
Limits LimitsOf(int Level, std::uint64_t NowUs, std::uint64_t Seed);

class Engine {
public:
    Engine();

    /* The best move it finds from At within Of; Stop set from elsewhere ends
     * the search at once. None when At has no move. */
    Move Think(Position At, const Limits &Of, const std::atomic<bool> &Stop);

private:
    struct Entry {
        std::uint64_t Key   = 0;
        Move          Best;
        std::int16_t  Score = 0;
        std::int8_t   Depth = -1;
        std::uint8_t  Bound = 0;
    };

    int  Search(Position &At, int Depth, int Alpha, int Beta, int Ply, bool MayPass);
    int  Quiet(Position &At, int Alpha, int Beta, int Ply);
    int  Weigh(const Position &At) const;
    void Order(const Position &At, MoveList &Moves, std::array<int, 256> &Worth, Move First, int Ply) const;
    bool Out();

    std::vector<Entry>                Table;
    std::array<std::array<Move, 2>, 128> Killers{};
    std::array<std::array<int, 128>, 16> Liked{};   // a quiet move's cuts, by its piece and square
    Limits                            Now;
    Move                              RootBest;   // the best move found at the root in the depth under way
    const std::atomic<bool>          *Stopped = nullptr;
    std::uint64_t                     Nodes   = 0;
    bool                              Over    = false;
};

}  // namespace chess

#endif
