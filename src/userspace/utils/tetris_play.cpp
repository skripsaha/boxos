/* tetris_play.cpp — the game itself: the well, the pieces, the rules
 * (tetris.h). Nothing here knows a screen or a key. */

#include <algorithm>
#include <cmath>

#include "tetris.h"

namespace {

using Cells = std::array<std::array<int, 2>, 4>;

/* The Super Rotation System's shapes: spawn, right, twice, left; I and O in a
 * 4x4 box, the rest in the top-left 3x3 of it. */
const Cells Shapes[7][4] = {
    {{{{1, 0}, {1, 1}, {1, 2}, {1, 3}}}, {{{0, 2}, {1, 2}, {2, 2}, {3, 2}}},
     {{{2, 0}, {2, 1}, {2, 2}, {2, 3}}}, {{{0, 1}, {1, 1}, {2, 1}, {3, 1}}}},
    {{{{0, 1}, {0, 2}, {1, 1}, {1, 2}}}, {{{0, 1}, {0, 2}, {1, 1}, {1, 2}}},
     {{{0, 1}, {0, 2}, {1, 1}, {1, 2}}}, {{{0, 1}, {0, 2}, {1, 1}, {1, 2}}}},
    {{{{0, 1}, {1, 0}, {1, 1}, {1, 2}}}, {{{0, 1}, {1, 1}, {1, 2}, {2, 1}}},
     {{{1, 0}, {1, 1}, {1, 2}, {2, 1}}}, {{{0, 1}, {1, 0}, {1, 1}, {2, 1}}}},
    {{{{0, 1}, {0, 2}, {1, 0}, {1, 1}}}, {{{0, 1}, {1, 1}, {1, 2}, {2, 2}}},
     {{{1, 1}, {1, 2}, {2, 0}, {2, 1}}}, {{{0, 0}, {1, 0}, {1, 1}, {2, 1}}}},
    {{{{0, 0}, {0, 1}, {1, 1}, {1, 2}}}, {{{0, 2}, {1, 1}, {1, 2}, {2, 1}}},
     {{{1, 0}, {1, 1}, {2, 1}, {2, 2}}}, {{{0, 1}, {1, 0}, {1, 1}, {2, 0}}}},
    {{{{0, 0}, {1, 0}, {1, 1}, {1, 2}}}, {{{0, 1}, {0, 2}, {1, 1}, {2, 1}}},
     {{{1, 0}, {1, 1}, {1, 2}, {2, 2}}}, {{{0, 1}, {1, 1}, {2, 0}, {2, 1}}}},
    {{{{0, 2}, {1, 0}, {1, 1}, {1, 2}}}, {{{0, 1}, {1, 1}, {2, 1}, {2, 2}}},
     {{{1, 0}, {1, 1}, {1, 2}, {2, 0}}}, {{{0, 0}, {0, 1}, {1, 1}, {2, 1}}}},
};

/* The kicks tried in turn when a turn does not fit, as SRS spells them: x to
 * the right, y up. [from][0] turning clockwise, [from][1] the other way. */
const int KicksJLSTZ[4][2][5][2] = {
    {{{0, 0}, {-1, 0}, {-1, 1}, {0, -2}, {-1, -2}}, {{0, 0}, {1, 0}, {1, 1}, {0, -2}, {1, -2}}},
    {{{0, 0}, {1, 0}, {1, -1}, {0, 2}, {1, 2}}, {{0, 0}, {1, 0}, {1, -1}, {0, 2}, {1, 2}}},
    {{{0, 0}, {1, 0}, {1, 1}, {0, -2}, {1, -2}}, {{0, 0}, {-1, 0}, {-1, 1}, {0, -2}, {-1, -2}}},
    {{{0, 0}, {-1, 0}, {-1, -1}, {0, 2}, {-1, 2}}, {{0, 0}, {-1, 0}, {-1, -1}, {0, 2}, {-1, 2}}},
};
const int KicksI[4][2][5][2] = {
    {{{0, 0}, {-2, 0}, {1, 0}, {-2, -1}, {1, 2}}, {{0, 0}, {-1, 0}, {2, 0}, {-1, 2}, {2, -1}}},
    {{{0, 0}, {-1, 0}, {2, 0}, {-1, 2}, {2, -1}}, {{0, 0}, {2, 0}, {-1, 0}, {2, 1}, {-1, -2}}},
    {{{0, 0}, {2, 0}, {-1, 0}, {2, 1}, {-1, -2}}, {{0, 0}, {1, 0}, {-2, 0}, {1, -2}, {-2, 1}}},
    {{{0, 0}, {1, 0}, {-2, 0}, {1, -2}, {-2, 1}}, {{0, 0}, {-2, 0}, {1, 0}, {-2, -1}, {1, 2}}},
};

constexpr float    LockSeconds  = 0.5f;
constexpr int      MostResets   = 15;
constexpr float    FlashSeconds = 0.3f;
const unsigned     LineScores[] = {0, 100, 300, 500, 800};

}

const std::array<std::array<int, 2>, 4> &PieceCells(Piece Kind, int Turn)
{
    return Shapes[static_cast<int>(Kind)][Turn & 3];
}

std::uint64_t Well::Roll()
{
    Seed ^= Seed << 13;
    Seed ^= Seed >> 7;
    Seed ^= Seed << 17;
    return Seed;
}

void Well::Start(std::uint64_t Seeded)
{
    for (auto &Row : Grid) Row.fill(Piece::None);
    Seed = Seeded | 1;
    Queue.clear();
    Bag.clear();
    Holding  = Piece::None;
    HoldOpen = true;
    Score = Lines = 0;
    Level = 1;
    Over  = false;
    Full.clear();
    FlashLeft = 0;
    Spawn();
}

/* The next piece, from a bag of all seven shaken anew when it is empty. */
Piece Well::Draw()
{
    while (Queue.size() <= static_cast<std::size_t>(NextShown)) {
        if (Bag.empty()) {
            for (int Kind = 0; Kind < 7; Kind++) Bag.push_back(static_cast<Piece>(Kind));
            for (std::size_t At = Bag.size() - 1; At > 0; At--)
                std::swap(Bag[At], Bag[static_cast<std::size_t>(Roll() % (At + 1))]);
        }
        Queue.push_back(Bag.back());
        Bag.pop_back();
    }
    Piece Next = Queue.front();
    Queue.erase(Queue.begin());
    return Next;
}

bool Well::Fits(const Falling &Piece) const
{
    for (const auto &Cell : PieceCells(Piece.Kind, Piece.Turn)) {
        int Row = Piece.Row + Cell[0], Col = Piece.Col + Cell[1];
        if (Row < 0 || Row >= WellHigh + WellHidden || Col < 0 || Col >= WellWide) return false;
        if (Grid[Row][Col] != Piece::None) return false;
    }
    return true;
}

void Well::Spawn()
{
    Current = Falling{Draw(), 0, 0, 3};
    Fall = Grounded = 0;
    Resets = 0;
    if (!Fits(Current)) {
        Over = true;
        return;
    }
    Falling Lower = Current;
    Lower.Row++;
    if (Fits(Lower)) Current = Lower;
}

float Well::FallSeconds() const
{
    float Base = std::max(0.05f, 0.8f - static_cast<float>(Level - 1) * 0.007f);
    return std::pow(Base, static_cast<float>(Level - 1));
}

/* The piece set into the well; full rows begin to flash. */
void Well::Lock()
{
    bool Above = true;
    for (const auto &Cell : PieceCells(Current.Kind, Current.Turn)) {
        Grid[Current.Row + Cell[0]][Current.Col + Cell[1]] = Current.Kind;
        Above = Above && Current.Row + Cell[0] < WellHidden;
    }
    Current.Kind = Piece::None;
    HoldOpen     = true;
    if (Above) {
        Over = true;
        return;
    }
    for (int Row = 0; Row < WellHigh + WellHidden; Row++) {
        bool Whole = true;
        for (Piece Cell : Grid[Row]) Whole = Whole && Cell != Piece::None;
        if (Whole) Full.push_back(Row);
    }
    if (Full.empty()) Spawn();
    else FlashLeft = FlashSeconds;
}

void Well::Pass(float Seconds)
{
    if (Over) return;
    if (!Full.empty()) {
        FlashLeft -= Seconds;
        if (FlashLeft > 0) return;
        for (int Row : Full) {
            for (int Above = Row; Above > 0; Above--) Grid[Above] = Grid[Above - 1];
            Grid[0].fill(Piece::None);
        }
        Score += LineScores[std::min<std::size_t>(Full.size(), 4)] * Level;
        Lines += static_cast<unsigned>(Full.size());
        Level  = Lines / 10 + 1;
        Full.clear();
        Spawn();
        return;
    }
    if (Current.Kind == Piece::None) return;

    Falling Lower = Current;
    Lower.Row++;
    if (!Fits(Lower)) {
        Grounded += Seconds;
        if (Grounded >= LockSeconds) Lock();
        return;
    }
    Grounded = 0;
    for (Fall += Seconds; Fall >= FallSeconds(); Fall -= FallSeconds()) {
        Lower = Current;
        Lower.Row++;
        if (!Fits(Lower)) {
            Fall = 0;
            break;
        }
        Current = Lower;
    }
}

bool Well::Shift(int Columns)
{
    if (Over || Current.Kind == Piece::None) return false;
    Falling Moved = Current;
    Moved.Col += Columns;
    if (!Fits(Moved)) return false;
    Current = Moved;
    if (Grounded > 0 && Resets < MostResets) {
        Grounded = 0;
        Resets++;
    }
    return true;
}

bool Well::SoftDrop()
{
    if (Over || Current.Kind == Piece::None) return false;
    Falling Lower = Current;
    Lower.Row++;
    if (!Fits(Lower)) return false;
    Current = Lower;
    Fall    = 0;
    Score  += 1;
    return true;
}

void Well::HardDrop()
{
    if (Over || Current.Kind == Piece::None) return;
    int Row = GhostRow();
    Score += static_cast<unsigned>(2 * (Row - Current.Row));
    Current.Row = Row;
    Lock();
}

int Well::GhostRow() const
{
    Falling Lower = Current;
    if (Lower.Kind == Piece::None) return Current.Row;
    while (true) {
        Lower.Row++;
        if (!Fits(Lower)) return Lower.Row - 1;
    }
}

bool Well::Turn(int Way)
{
    if (Over || Current.Kind == Piece::None || Current.Kind == Piece::O) return false;
    int From = Current.Turn, Side = Way > 0 ? 0 : 1;
    const int (*Kicks)[2] = Current.Kind == Piece::I ? KicksI[From][Side] : KicksJLSTZ[From][Side];
    for (int Try = 0; Try < 5; Try++) {
        Falling Turned = Current;
        Turned.Turn = (From + (Way > 0 ? 1 : 3)) % 4;
        Turned.Col += Kicks[Try][0];
        Turned.Row -= Kicks[Try][1];
        if (!Fits(Turned)) continue;
        Current = Turned;
        if (Grounded > 0 && Resets < MostResets) {
            Grounded = 0;
            Resets++;
        }
        return true;
    }
    return false;
}

void Well::Hold()
{
    if (Over || !HoldOpen || Current.Kind == Piece::None) return;
    Piece Was = Current.Kind;
    if (Holding == Piece::None) {
        Holding = Was;
        Spawn();
    } else {
        Current = Falling{Holding, 0, 0, 3};
        Holding = Was;
        Fall = Grounded = 0;
        Resets = 0;
        if (!Fits(Current)) Over = true;
    }
    HoldOpen = false;
}
