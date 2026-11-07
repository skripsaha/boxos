#ifndef TETRIS_H
#define TETRIS_H

/* tetris — the falling pieces. tetris_play.cpp is the game and takes nothing
 * of BoxOS; tetris.cpp is the glass, the keys and the record.
 *
 * The well is ten wide and twenty high, with two rows above it where a piece
 * is born. Pieces come in bags of all seven, turned by the Super Rotation
 * System, kicked off walls by its tables; a piece on the ground waits half a
 * second before it locks, and each move of it starts the wait again, fifteen
 * times at most. Lines score 100, 300, 500, 800 times the level; a soft drop a
 * point a row, a hard drop two. Every ten lines the level rises and the pieces
 * fall faster. */

#include <array>
#include <cstdint>
#include <vector>

constexpr int WellWide   = 10;
constexpr int WellHigh   = 20;
constexpr int WellHidden = 2;
constexpr int NextShown  = 3;

enum class Piece : std::uint8_t { I, O, T, S, Z, J, L, None };

struct Falling {
    Piece Kind = Piece::None;
    int   Turn = 0;     /* 0 spawn, 1 right, 2 twice, 3 left */
    int   Row  = 0;     /* the top of its 4x4 box, counting the hidden rows */
    int   Col  = 0;
};

/* The four cells of a piece turned so, in its 4x4 box: row, column pairs. */
const std::array<std::array<int, 2>, 4> &PieceCells(Piece Kind, int Turn);

class Well {
public:
    void Start(std::uint64_t Seed);

    /* Time passing: gravity, the lock wait, the cleared lines' flash. */
    void Pass(float Seconds);

    bool Shift(int Columns);
    bool SoftDrop();
    void HardDrop();
    bool Turn(int Way);         /* 1 clockwise, -1 the other way */
    void Hold();

    Piece Cell(int Row, int Col) const { return Grid[Row][Col]; }
    const Falling &Now() const { return Current; }
    int   GhostRow() const;
    Piece Held() const { return Holding; }
    bool  CanHold() const { return HoldOpen; }
    Piece Upcoming(int At) const { return Queue[static_cast<std::size_t>(At)]; }

    /* Rows being cleared, flashing; empty when none. */
    const std::vector<int> &Clearing() const { return Full; }
    float ClearShown() const { return FlashLeft; }

    unsigned Score = 0;
    unsigned Lines = 0;
    unsigned Level = 1;
    bool     Over  = false;

private:
    bool          Fits(const Falling &Piece) const;
    void          Spawn();
    Piece         Draw();
    void          Lock();
    float         FallSeconds() const;
    std::uint64_t Roll();

    std::array<std::array<Piece, WellWide>, WellHigh + WellHidden> Grid{};
    Falling           Current;
    std::vector<Piece> Queue;
    std::vector<Piece> Bag;
    Piece             Holding  = Piece::None;
    bool              HoldOpen = true;
    float             Fall     = 0;     /* seconds toward the next row */
    float             Grounded = 0;     /* seconds on the ground */
    int               Resets   = 0;
    std::vector<int>  Full;
    float             FlashLeft = 0;
    std::uint64_t     Seed     = 1;
};

#endif
