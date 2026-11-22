/* engine.cpp — the computer's judgement and its search. See engine.h. */

#include "engine.h"

#include <algorithm>
#include <cstdlib>

#include "box/clock.h"

namespace chess {

namespace {

constexpr int Mate      = 30000;
constexpr int MateBound = 29000;   // a score past this is a mate found, so many plies away
constexpr int Infinity  = 32000;
constexpr int MostPly   = 120;

constexpr std::size_t TableSize = std::size_t{1} << 19;

enum Bound : std::uint8_t { Exact = 1, Lower = 2, Upper = 3 };

constexpr int Worth[7] = {0, 100, 320, 330, 500, 900, 0};
constexpr int Phases[7] = {0, 0, 1, 1, 2, 4, 0};   // how much of the middle game a piece is: 24 all told

/* Where each piece stands best, from white's side, a1 to h8 rank by rank —
 * for a black piece the rank is read from the other end. */
using Table64 = std::array<std::int8_t, 64>;

constexpr Table64 PawnMiddle = {
    0,  0,  0,  0,  0,  0,  0,  0,     //
    0,  4,  4,  -8, -8, 4,  4,  0,     //
    2,  -2, -4, 4,  4,  -4, -2, 2,     //
    0,  0,  6,  18, 18, 6,  0,  0,     //
    4,  6,  10, 22, 22, 10, 6,  4,     //
    12, 16, 22, 28, 28, 22, 16, 12,    //
    40, 44, 48, 52, 52, 48, 44, 40,    //
    0,  0,  0,  0,  0,  0,  0,  0};
constexpr Table64 PawnEnd = {
    0,  0,  0,  0,  0,  0,  0,  0,     //
    0,  0,  0,  0,  0,  0,  0,  0,     //
    4,  4,  4,  4,  4,  4,  4,  4,     //
    8,  8,  8,  8,  8,  8,  8,  8,     //
    14, 14, 14, 14, 14, 14, 14, 14,    //
    24, 24, 24, 24, 24, 24, 24, 24,    //
    40, 40, 40, 40, 40, 40, 40, 40,    //
    0,  0,  0,  0,  0,  0,  0,  0};
constexpr Table64 KnightAll = {
    -45, -30, -22, -20, -20, -22, -30, -45,   //
    -30, -14, 0,   4,   4,   0,   -14, -30,   //
    -20, 4,   12,  16,  16,  12,  4,   -20,   //
    -16, 6,   18,  24,  24,  18,  6,   -16,   //
    -16, 8,   20,  26,  26,  20,  8,   -16,   //
    -20, 6,   16,  22,  22,  16,  6,   -20,   //
    -30, -12, 2,   6,   6,   2,   -12, -30,   //
    -50, -32, -24, -20, -20, -24, -32, -50};
constexpr Table64 BishopAll = {
    -18, -8,  -10, -8, -8, -10, -8,  -18,   //
    -8,  10,  4,   6,  6,  4,   10,  -8,    //
    -6,  8,   10,  10, 10, 10,  8,   -6,    //
    -4,  4,   12,  14, 14, 12,  4,   -4,    //
    -4,  6,   10,  14, 14, 10,  6,   -4,    //
    -6,  4,   8,   10, 10, 8,   4,   -6,    //
    -8,  0,   2,   4,  4,  2,   0,   -8,    //
    -18, -10, -10, -8, -8, -10, -10, -18};
constexpr Table64 RookMiddle = {
    -2, 0,  4,  8,  8,  4,  0,  -2,    //
    -8, -2, 0,  0,  0,  0,  -2, -8,    //
    -6, 0,  0,  0,  0,  0,  0,  -6,    //
    -6, 0,  0,  0,  0,  0,  0,  -6,    //
    -6, 0,  0,  0,  0,  0,  0,  -6,    //
    -6, 0,  0,  0,  0,  0,  0,  -6,    //
    12, 16, 16, 16, 16, 16, 16, 12,    //
    4,  4,  4,  6,  6,  4,  4,  4};
constexpr Table64 RookEnd = {
    0,  0,  0,  0,  0,  0,  0,  0,     //
    0,  0,  0,  0,  0,  0,  0,  0,     //
    0,  0,  0,  0,  0,  0,  0,  0,     //
    0,  0,  0,  0,  0,  0,  0,  0,     //
    2,  2,  2,  2,  2,  2,  2,  2,     //
    4,  4,  4,  4,  4,  4,  4,  4,     //
    10, 10, 10, 10, 10, 10, 10, 10,    //
    4,  4,  4,  4,  4,  4,  4,  4};
constexpr Table64 QueenAll = {
    -16, -8, -6, -2, -2, -6, -8, -16,   //
    -8,  0,  4,  2,  2,  2,  0,  -8,    //
    -6,  2,  4,  4,  4,  4,  2,  -6,    //
    -2,  0,  4,  6,  6,  4,  0,  -2,    //
    -2,  0,  4,  6,  6,  4,  0,  -2,    //
    -6,  0,  4,  4,  4,  4,  0,  -6,    //
    -8,  0,  0,  0,  0,  0,  0,  -8,    //
    -16, -8, -6, -4, -4, -6, -8, -16};
constexpr Table64 KingMiddle = {
    18,  28,  10,  -4,  0,   8,   30,  20,    //
    12,  12,  -6,  -18, -18, -10, 14,  14,    //
    -12, -20, -24, -30, -30, -24, -20, -12,   //
    -24, -32, -36, -44, -44, -36, -32, -24,   //
    -34, -42, -46, -54, -54, -46, -42, -34,   //
    -34, -42, -46, -54, -54, -46, -42, -34,   //
    -34, -42, -46, -54, -54, -46, -42, -34,   //
    -34, -42, -46, -54, -54, -46, -42, -34};
constexpr Table64 KingEnd = {
    -40, -26, -18, -14, -14, -18, -26, -40,   //
    -24, -10, -2,  2,   2,   -2,  -10, -24,   //
    -16, 0,   12,  16,  16,  12,  0,   -16,   //
    -12, 4,   18,  26,  26,  18,  4,   -12,   //
    -12, 4,   18,  26,  26,  18,  4,   -12,   //
    -16, 0,   12,  16,  16,  12,  0,   -16,   //
    -24, -10, -2,  2,   2,   -2,  -10, -24,   //
    -40, -26, -18, -14, -14, -18, -26, -40};

constexpr const Table64 *Middles[7] = {nullptr, &PawnMiddle, &KnightAll, &BishopAll, &RookMiddle, &QueenAll, &KingMiddle};
constexpr const Table64 *Ends[7]    = {nullptr, &PawnEnd, &KnightAll, &BishopAll, &RookEnd, &QueenAll, &KingEnd};

/* A passed pawn by how far it has come, 0 its home rank's side. */
constexpr int PassedMiddle[8] = {0, 4, 8, 14, 24, 40, 64, 0};
constexpr int PassedEnd[8]    = {0, 10, 16, 28, 48, 80, 120, 0};

int ToTable(int Score, int Ply)
{
    return Score >= MateBound ? Score + Ply : Score <= -MateBound ? Score - Ply : Score;
}

int FromTable(int Score, int Ply)
{
    return Score >= MateBound ? Score - Ply : Score <= -MateBound ? Score + Ply : Score;
}

/* The best of the moves from At on, brought to At. */
void PickBest(MoveList &Moves, std::array<int, 256> &Worths, int At)
{
    int Best = At;
    for (int Next = At + 1; Next < Moves.Count; Next++)
        if (Worths[static_cast<std::size_t>(Next)] > Worths[static_cast<std::size_t>(Best)]) Best = Next;
    std::swap(Moves.At[static_cast<std::size_t>(At)], Moves.At[static_cast<std::size_t>(Best)]);
    std::swap(Worths[static_cast<std::size_t>(At)], Worths[static_cast<std::size_t>(Best)]);
}

}  // namespace

Limits LimitsOf(int Level, std::uint64_t NowUs, std::uint64_t Seed)
{
    struct Of {
        int           Depth;
        int           Whim;
        std::uint64_t Us;
    };
    constexpr Of ByLevel[Levels] = {{1, 160, 500000}, {2, 70, 600000}, {3, 30, 800000},
                                    {64, 8, 1000000}, {64, 4, 2000000}, {64, 2, 4000000}};
    const Of &One = ByLevel[std::clamp(Level, 1, Levels) - 1];
    return Limits{One.Depth, NowUs + One.Us, One.Whim, Seed};
}

Engine::Engine() : Table(TableSize) {}

bool Engine::Out()
{
    if (Over) return true;
    if ((++Nodes & 1023) == 0 && (Stopped->load(std::memory_order_relaxed) || clock_uptime_us() >= Now.DueUs)) Over = true;
    return Over;
}

int Engine::Weigh(const Position &At) const
{
    int Middle[2] = {0, 0}, End[2] = {0, 0}, Phase = 0, Bishops[2] = {0, 0};
    int Count[2][8] = {}, Lowest[2][8], Highest[2][8];
    for (int File = 0; File < 8; File++)
        for (int Side_ = 0; Side_ < 2; Side_++) {
            Lowest[Side_][File]  = 8;
            Highest[Side_][File] = -1;
        }
    for (Square Of = 0; Of < 128; Of++) {
        if (!OnBoard(Of)) {
            Of += 7;
            continue;
        }
        const Piece One = At.At(Of);
        if (One == 0) continue;
        const Side  Mine = SideOf(One);
        const Kind  Is   = KindOf(One);
        const std::size_t Cell = static_cast<std::size_t>(Mine == White ? Index(Of) : Index(Of) ^ 56);
        Middle[Mine] += Worth[Is] + (*Middles[Is])[Cell];
        End[Mine] += Worth[Is] + (*Ends[Is])[Cell];
        Phase += Phases[Is];
        if (Is == Bishop) Bishops[Mine]++;
        if (Is == Pawn) {
            const int File = FileOf(Of), Rank = RankOf(Of);
            Count[Mine][File]++;
            Lowest[Mine][File]  = std::min(Lowest[Mine][File], Rank);
            Highest[Mine][File] = std::max(Highest[Mine][File], Rank);
        }
    }
    for (Square Of = 0; Of < 128; Of++) {
        if (!OnBoard(Of)) {
            Of += 7;
            continue;
        }
        const Piece One = At.At(Of);
        if (KindOf(One) != Pawn && KindOf(One) != Rook) continue;
        const Side Mine = SideOf(One), Them = Other(Mine);
        const int  File = FileOf(Of), Rank = RankOf(Of);
        if (KindOf(One) == Rook) {
            if (Count[Mine][File] == 0) {
                const bool Open = Count[Them][File] == 0;
                Middle[Mine] += Open ? 20 : 10;
                End[Mine] += Open ? 10 : 5;
            }
            continue;
        }
        const bool Alone = (File == 0 || Count[Mine][File - 1] == 0) && (File == 7 || Count[Mine][File + 1] == 0);
        if (Alone) {
            Middle[Mine] -= 14;
            End[Mine] -= 10;
        }
        /* Passed: no pawn of theirs ahead of it on its file or either side. */
        bool Passed = true;
        for (int Near = std::max(0, File - 1); Near <= std::min(7, File + 1); Near++)
            Passed &= Mine == White ? Highest[Them][Near] <= Rank : Lowest[Them][Near] >= Rank;
        if (Passed) {
            const int Come = Mine == White ? Rank : 7 - Rank;
            Middle[Mine] += PassedMiddle[Come];
            End[Mine] += PassedEnd[Come];
        }
    }
    for (int Side_ = 0; Side_ < 2; Side_++) {
        for (int File = 0; File < 8; File++)
            if (Count[Side_][File] > 1) {
                Middle[Side_] -= 12 * (Count[Side_][File] - 1);
                End[Side_] -= 18 * (Count[Side_][File] - 1);
            }
        if (Bishops[Side_] >= 2) {
            Middle[Side_] += 30;
            End[Side_] += 40;
        }
        /* The pawns before a king tucked into a corner. */
        const Square King_ = At.KingOf(static_cast<Side>(Side_));
        const int    Home  = Side_ == White ? 0 : 7, Ahead = Side_ == White ? 16 : -16;
        if (RankOf(King_) == Home && (FileOf(King_) <= 2 || FileOf(King_) >= 5))
            for (int Near = std::max(0, FileOf(King_) - 1); Near <= std::min(7, FileOf(King_) + 1); Near++) {
                const Square Before = SquareAt(Near, Home) + Ahead;
                if (At.At(Before) == PieceOf(static_cast<Side>(Side_), Pawn)) Middle[Side_] += 10;
                else if (At.At(Before + Ahead) == PieceOf(static_cast<Side>(Side_), Pawn)) Middle[Side_] += 5;
            }
    }
    Phase           = std::min(Phase, 24);
    const int Mid   = Middle[White] - Middle[Black], Late = End[White] - End[Black];
    int       Score = (Mid * Phase + Late * (24 - Phase)) / 24;
    if (At.ToMove() == Black) Score = -Score;
    Score += 10;
    if (Now.Whim > 0) {
        std::uint64_t Mixed = (At.Key() ^ Now.Seed) * 0x9E3779B97F4A7C15ull;
        Mixed ^= Mixed >> 29;
        Score += static_cast<int>(Mixed % static_cast<std::uint64_t>(2 * Now.Whim + 1)) - Now.Whim;
    }
    return Score;
}

void Engine::Order(const Position &At, MoveList &Moves, std::array<int, 256> &Worths, Move First, int Ply) const
{
    for (int I = 0; I < Moves.Count; I++) {
        const Move  One   = Moves.At[static_cast<std::size_t>(I)];
        const Piece Mover = At.At(One.From);
        int         Value = Liked[Mover][One.To];
        if (One == First) Value = 1000000;
        else if (One.Is(Move::Capture)) Value = 100000 + 16 * Worth[One.Is(Move::InPassing) ? Pawn : KindOf(At.At(One.To))] - KindOf(Mover);
        else if (One.Promote != NoKind) Value = 90000 + Worth[One.Promote];
        else if (Ply < 128 && One == Killers[static_cast<std::size_t>(Ply)][0]) Value = 80000;
        else if (Ply < 128 && One == Killers[static_cast<std::size_t>(Ply)][1]) Value = 79000;
        Worths[static_cast<std::size_t>(I)] = Value;
    }
}

int Engine::Quiet(Position &At, int Alpha, int Beta, int Ply)
{
    if (Out()) return 0;
    const bool Check = At.InCheck();
    const int  Stand = Weigh(At);
    if (Ply >= MostPly) return Stand;
    int Best = -Infinity;
    /* In check there is no standing still: every way out is tried. */
    if (!Check) {
        if (Stand >= Beta) return Stand;
        Alpha = std::max(Alpha, Stand);
        Best  = Stand;
    }
    MoveList Moves;
    At.Candidates(Moves, !Check);
    std::array<int, 256> Worths;
    Order(At, Moves, Worths, Move{}, MostPly);
    int Legal = 0;
    for (int I = 0; I < Moves.Count; I++) {
        PickBest(Moves, Worths, I);
        const Move One = Moves.At[static_cast<std::size_t>(I)];
        /* A capture that could not lift the score to alpha even winning its
         * piece outright is not looked at. */
        if (!Check && One.Promote == NoKind &&
            Stand + Worth[One.Is(Move::InPassing) ? Pawn : KindOf(At.At(One.To))] + 200 < Alpha)
            continue;
        if (!At.Play(One)) continue;
        Legal++;
        const int Score = -Quiet(At, -Beta, -Alpha, Ply + 1);
        At.Undo();
        if (Over) return 0;
        Best = std::max(Best, Score);
        Alpha = std::max(Alpha, Score);
        if (Alpha >= Beta) break;
    }
    if (Check && Legal == 0) return -Mate + Ply;
    return Best;
}

int Engine::Search(Position &At, int Depth, int Alpha, int Beta, int Ply, bool MayPass)
{
    if (Out()) return 0;
    const bool Root = Ply == 0;
    if (!Root && (At.SinceCaptureOrPawn() >= 100 || At.Repeated(1))) return 0;
    if (Ply >= MostPly) return Weigh(At);
    const bool Check = At.InCheck();
    if (Check) Depth++;
    if (Depth <= 0) return Quiet(At, Alpha, Beta, Ply);

    Alpha = std::max(Alpha, -Mate + Ply);
    Beta  = std::min(Beta, Mate - Ply - 1);
    if (Alpha >= Beta) return Alpha;

    Entry &Slot  = Table[At.Key() & (TableSize - 1)];
    Move   First = {};
    if (Slot.Key == At.Key()) {
        First = Slot.Best;
        if (!Root && Slot.Depth >= Depth) {
            const int Kept = FromTable(Slot.Score, Ply);
            if (Slot.Bound == Exact || (Slot.Bound == Lower && Kept >= Beta) || (Slot.Bound == Upper && Kept <= Alpha)) return Kept;
        }
    }

    const bool Wide = Beta - Alpha > 1;
    /* Even passing the move it stays above beta: it will not fall below by
     * moving. Never with only pawns, where a move can be a loss. */
    if (MayPass && !Wide && !Check && Depth >= 3 && At.HasPieces(At.ToMove()) && Weigh(At) >= Beta) {
        At.Pass();
        const int Score = -Search(At, Depth - 1 - (Depth >= 6 ? 3 : 2), -Beta, -Beta + 1, Ply + 1, false);
        At.Unpass();
        if (Over) return 0;
        if (Score >= Beta) return Score >= MateBound ? Beta : Score;
    }

    MoveList Moves;
    At.Candidates(Moves);
    std::array<int, 256> Worths;
    Order(At, Moves, Worths, First, Ply);
    const int AlphaWas = Alpha;
    int       Best     = -Infinity, Legal = 0;
    Move      BestMove = {};
    for (int I = 0; I < Moves.Count; I++) {
        PickBest(Moves, Worths, I);
        const Move  One   = Moves.At[static_cast<std::size_t>(I)];
        const Piece Mover = At.At(One.From);
        if (!At.Play(One)) continue;
        Legal++;
        const bool Calm = !One.Is(Move::Capture) && One.Promote == NoKind;
        int        Score;
        if (Legal == 1) Score = -Search(At, Depth - 1, -Beta, -Alpha, Ply + 1, true);
        else {
            /* Late quiet moves are rarely best: looked at shallower first,
             * and again at full depth only if they surprise. */
            const int Less = Calm && Depth >= 3 && Legal > 3 && !Check && !At.InCheck() ? (Legal > 8 ? 2 : 1) : 0;
            Score = -Search(At, Depth - 1 - Less, -Alpha - 1, -Alpha, Ply + 1, true);
            if (Score > Alpha && Less > 0) Score = -Search(At, Depth - 1, -Alpha - 1, -Alpha, Ply + 1, true);
            if (Score > Alpha && Score < Beta) Score = -Search(At, Depth - 1, -Beta, -Alpha, Ply + 1, true);
        }
        At.Undo();
        if (Over) return 0;
        if (Score > Best) {
            Best     = Score;
            BestMove = One;
            if (Root) RootBest = One;
        }
        Alpha = std::max(Alpha, Score);
        if (Alpha >= Beta) {
            if (Calm && Ply < 128) {
                auto &Two = Killers[static_cast<std::size_t>(Ply)];
                if (!(Two[0] == One)) {
                    Two[1] = Two[0];
                    Two[0] = One;
                }
                Liked[Mover][One.To] = std::min(Liked[Mover][One.To] + Depth * Depth, 60000);
            }
            break;
        }
    }
    if (Legal == 0) return Check ? -Mate + Ply : 0;
    Slot.Key   = At.Key();
    Slot.Best  = BestMove;
    Slot.Score = static_cast<std::int16_t>(ToTable(Best, Ply));
    Slot.Depth = static_cast<std::int8_t>(std::min(Depth, 127));
    Slot.Bound = Best >= Beta ? Lower : Best > AlphaWas ? Exact : Upper;
    return Best;
}

Move Engine::Think(Position At, const Limits &Of, const std::atomic<bool> &Stop)
{
    Now     = Of;
    Stopped = &Stop;
    Nodes   = 0;
    Over    = false;
    Killers = {};
    for (auto &Row : Liked)
        for (int &One : Row) One /= 4;
    MoveList Legal;
    At.Moves(Legal);
    if (Legal.Count == 0) return {};
    Move Chosen = Legal.At[0];
    if (Legal.Count == 1) return Chosen;
    for (int Depth = 1; Depth <= Of.Depth; Depth++) {
        RootBest        = {};
        const int Score = Search(At, Depth, -Infinity, Infinity, 0, false);
        /* Even of a depth cut short, the best so far was searched whole. */
        if (RootBest) Chosen = RootBest;
        if (Over || std::abs(Score) >= MateBound) break;
    }
    return Chosen;
}

}  // namespace chess
