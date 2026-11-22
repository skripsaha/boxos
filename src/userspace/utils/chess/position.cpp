/* position.cpp — the rules of chess. See position.h. */

#include "position.h"

namespace chess {

namespace {

constexpr int KnightSteps[8] = {33, 31, 18, 14, -33, -31, -18, -14};
constexpr int KingSteps[8]   = {1, -1, 16, -16, 15, 17, -15, -17};
constexpr int Diagonals[4]   = {15, 17, -15, -17};
constexpr int Straights[4]   = {1, -1, 16, -16};

constexpr Kind Promotions[4] = {Queen, Rook, Bishop, Knight};

/* The castlings a move from or to a square takes away: a king's, a rook's. */
constexpr std::array<std::uint8_t, 128> CastlingKept = [] {
    std::array<std::uint8_t, 128> Made{};
    Made.fill(15);
    Made[SquareAt(4, 0)] = 15 & ~3;
    Made[SquareAt(0, 0)] = 15 & ~2;
    Made[SquareAt(7, 0)] = 15 & ~1;
    Made[SquareAt(4, 7)] = 15 & ~12;
    Made[SquareAt(0, 7)] = 15 & ~8;
    Made[SquareAt(7, 7)] = 15 & ~4;
    return Made;
}();

/* The numbers a position's key is made of, the same every game: a piece on a
 * square, the side to move, the castlings, the file a pawn may be taken on. */
struct Keys {
    std::array<std::array<std::uint64_t, 64>, 16> Pieces{};
    std::uint64_t                                 BlackToMove = 0;
    std::array<std::uint64_t, 16>                 Castlings{};
    std::array<std::uint64_t, 8>                  Files{};
};

constexpr Keys MadeKeys()
{
    Keys          Made;
    std::uint64_t State = 0x9E3779B97F4A7C15ull;
    const auto    Next  = [&State] {
        State ^= State << 13;
        State ^= State >> 7;
        State ^= State << 17;
        return State;
    };
    for (auto &Row : Made.Pieces)
        for (std::uint64_t &One : Row) One = Next();
    Made.BlackToMove = Next();
    for (std::uint64_t &One : Made.Castlings) One = Next();
    for (std::uint64_t &One : Made.Files) One = Next();
    return Made;
}

constexpr Keys Zobrist = MadeKeys();

std::uint64_t KeyOf(Piece One, Square Of)
{
    return Zobrist.Pieces[One][static_cast<std::size_t>(Index(Of))];
}

constexpr char Letters[7] = {' ', 'P', 'N', 'B', 'R', 'Q', 'K'};

}  // namespace

std::string SquareName(Square Of)
{
    return {static_cast<char>('a' + FileOf(Of)), static_cast<char>('1' + RankOf(Of))};
}

Position::Position()
{
    constexpr Kind Back[8] = {Rook, Knight, Bishop, Queen, King, Bishop, Knight, Rook};
    for (int File = 0; File < 8; File++) {
        Put(SquareAt(File, 0), PieceOf(White, Back[File]));
        Put(SquareAt(File, 1), PieceOf(White, Pawn));
        Put(SquareAt(File, 6), PieceOf(Black, Pawn));
        Put(SquareAt(File, 7), PieceOf(Black, Back[File]));
    }
    Hash ^= Zobrist.Castlings[Castling];
    History.reserve(512);
}

void Position::Put(Square Of, Piece One)
{
    Board[static_cast<std::size_t>(Of)] = One;
    Hash ^= KeyOf(One, Of);
    if (KindOf(One) == King) Kings[SideOf(One)] = Of;
}

void Position::Lift(Square Of)
{
    Hash ^= KeyOf(Board[static_cast<std::size_t>(Of)], Of);
    Board[static_cast<std::size_t>(Of)] = 0;
}

bool Position::Attacked(Square Of, Side By) const
{
    /* A pawn takes forward and to the side: the squares behind Of, as By
     * moves, are where one would stand. */
    const int Behind = By == White ? -16 : 16;
    for (const int Side : {-1, 1}) {
        const int From = Of + Behind + Side;
        if (OnBoard(From) && Board[static_cast<std::size_t>(From)] == PieceOf(By, Pawn)) return true;
    }
    for (const int Step : KnightSteps) {
        const int From = Of + Step;
        if (OnBoard(From) && Board[static_cast<std::size_t>(From)] == PieceOf(By, Knight)) return true;
    }
    for (const int Step : KingSteps) {
        const int From = Of + Step;
        if (OnBoard(From) && Board[static_cast<std::size_t>(From)] == PieceOf(By, King)) return true;
    }
    const auto Slides = [&](const int (&Steps)[4], Kind Along) {
        for (const int Step : Steps)
            for (int From = Of + Step; OnBoard(From); From += Step) {
                const Piece One = Board[static_cast<std::size_t>(From)];
                if (One == 0) continue;
                if (SideOf(One) == By && (KindOf(One) == Along || KindOf(One) == Queen)) return true;
                break;
            }
        return false;
    };
    return Slides(Diagonals, Bishop) || Slides(Straights, Rook);
}

void Position::Pawns(MoveList &Into, Square From, bool Captures) const
{
    const int  Ahead    = Turn == White ? 16 : -16;
    const int  LastRank = Turn == White ? 7 : 0;
    const int  HomeRank = Turn == White ? 1 : 6;
    const auto Add      = [&](int To, std::uint8_t Flags) {
        if (RankOf(To) == LastRank)
            for (const Kind Into_ : Promotions) Into.Add(Move{static_cast<std::uint8_t>(From), static_cast<std::uint8_t>(To), Into_, Flags});
        else Into.Add(Move{static_cast<std::uint8_t>(From), static_cast<std::uint8_t>(To), NoKind, Flags});
    };
    for (const int Side : {-1, 1}) {
        const int To = From + Ahead + Side;
        if (!OnBoard(To)) continue;
        const Piece There = Board[static_cast<std::size_t>(To)];
        if (There != 0 && SideOf(There) != Turn) Add(To, Move::Capture);
        else if (To == InPassing) Into.Add(Move{static_cast<std::uint8_t>(From), static_cast<std::uint8_t>(To), NoKind, Move::Capture | Move::InPassing});
    }
    const int One = From + Ahead;
    if (!OnBoard(One) || Board[static_cast<std::size_t>(One)] != 0) return;
    /* Among the captures go the promotions, which win as much. */
    if (Captures) {
        if (RankOf(One) == LastRank) Add(One, 0);
        return;
    }
    Add(One, 0);
    const int Two = One + Ahead;
    if (RankOf(From) == HomeRank && Board[static_cast<std::size_t>(Two)] == 0)
        Into.Add(Move{static_cast<std::uint8_t>(From), static_cast<std::uint8_t>(Two), NoKind, Move::Double});
}

void Position::Castlings(MoveList &Into) const
{
    const int     Rank  = Turn == White ? 0 : 7;
    const Square  King_ = SquareAt(4, Rank);
    const Side    Them  = Other(Turn);
    const std::uint8_t Short = Turn == White ? 1 : 4, Long = Turn == White ? 2 : 8;
    if ((Castling & (Short | Long)) == 0 || Kings[Turn] != King_ || Attacked(King_, Them)) return;
    const auto Empty = [&](int File) { return Board[static_cast<std::size_t>(SquareAt(File, Rank))] == 0; };
    const auto Safe  = [&](int File) { return !Attacked(SquareAt(File, Rank), Them); };
    if ((Castling & Short) && Empty(5) && Empty(6) && Safe(5) && Safe(6))
        Into.Add(Move{static_cast<std::uint8_t>(King_), static_cast<std::uint8_t>(SquareAt(6, Rank)), NoKind, Move::Castle});
    if ((Castling & Long) && Empty(1) && Empty(2) && Empty(3) && Safe(3) && Safe(2))
        Into.Add(Move{static_cast<std::uint8_t>(King_), static_cast<std::uint8_t>(SquareAt(2, Rank)), NoKind, Move::Castle});
}

void Position::Candidates(MoveList &Into, bool Captures) const
{
    Into.Count = 0;
    for (Square From = 0; From < 128; From++) {
        if (!OnBoard(From)) {
            From += 7;
            continue;
        }
        const Piece One = Board[static_cast<std::size_t>(From)];
        if (One == 0 || SideOf(One) != Turn) continue;
        const Kind Is = KindOf(One);
        if (Is == Pawn) {
            Pawns(Into, From, Captures);
            continue;
        }
        const auto Reach = [&](int To) {
            const Piece There = Board[static_cast<std::size_t>(To)];
            if (There == 0) {
                if (!Captures) Into.Add(Move{static_cast<std::uint8_t>(From), static_cast<std::uint8_t>(To), NoKind, 0});
                return true;
            }
            if (SideOf(There) != Turn) Into.Add(Move{static_cast<std::uint8_t>(From), static_cast<std::uint8_t>(To), NoKind, Move::Capture});
            return false;
        };
        if (Is == Knight || Is == King) {
            for (const int Step : Is == Knight ? KnightSteps : KingSteps)
                if (OnBoard(From + Step)) (void)Reach(From + Step);
            continue;
        }
        const auto Slide = [&](const int (&Steps)[4]) {
            for (const int Step : Steps)
                for (int To = From + Step; OnBoard(To) && Reach(To); To += Step) {}
        };
        if (Is != Rook) Slide(Diagonals);
        if (Is != Bishop) Slide(Straights);
    }
    if (!Captures) Castlings(Into);
}

void Position::Moves(MoveList &Into) const
{
    MoveList All;
    Position Trying = *this;
    Candidates(All);
    Into.Count = 0;
    for (const Move One : All)
        if (Trying.Play(One)) {
            Trying.Undo();
            Into.Add(One);
        }
}

bool Position::CanTakeInPassing() const
{
    const int Behind = Turn == White ? -16 : 16;
    for (const int Side : {-1, 1}) {
        const int From = InPassing + Behind + Side;
        if (OnBoard(From) && Board[static_cast<std::size_t>(From)] == PieceOf(Turn, Pawn)) return true;
    }
    return false;
}

bool Position::Play(Move One)
{
    const Square From = One.From, To = One.To;
    const Piece  Mover = Board[From];
    const Side   Us    = Turn;
    Was          Kept{One, Board[To], Castling, InPassing, Fifty, Hash};

    if (InPassing >= 0) Hash ^= Zobrist.Files[static_cast<std::size_t>(FileOf(InPassing))];
    if (One.Is(Move::InPassing)) {
        const Square Taken = To + (Us == White ? -16 : 16);
        Kept.Taken         = Board[static_cast<std::size_t>(Taken)];
        Lift(Taken);
    } else if (Board[To] != 0) Lift(To);
    Lift(From);
    Put(To, One.Promote != NoKind ? PieceOf(Us, One.Promote) : Mover);
    if (One.Is(Move::Castle)) {
        const bool   Short = FileOf(To) == 6;
        const Square Rook_ = SquareAt(Short ? 7 : 0, RankOf(To));
        const Piece  Moved = Board[static_cast<std::size_t>(Rook_)];
        Lift(Rook_);
        Put(SquareAt(Short ? 5 : 3, RankOf(To)), Moved);
    }
    Hash ^= Zobrist.Castlings[Castling];
    Castling &= CastlingKept[From] & CastlingKept[To];
    Hash ^= Zobrist.Castlings[Castling];
    Fifty = KindOf(Mover) == Pawn || Kept.Taken != 0 ? 0 : Fifty + 1;
    Turn  = Other(Us);
    Hash ^= Zobrist.BlackToMove;
    InPassing = One.Is(Move::Double) ? (From + To) / 2 : -1;
    if (InPassing >= 0 && !CanTakeInPassing()) InPassing = -1;
    if (InPassing >= 0) Hash ^= Zobrist.Files[static_cast<std::size_t>(FileOf(InPassing))];
    History.push_back(Kept);
    if (Attacked(Kings[Us], Turn)) {
        Undo();
        return false;
    }
    return true;
}

void Position::Undo()
{
    const Was Kept = History.back();
    History.pop_back();
    const Move One = Kept.Played;
    Turn           = Other(Turn);
    const Piece Moved = One.Promote != NoKind ? PieceOf(Turn, Pawn) : Board[One.To];
    Board[One.To]     = 0;
    Board[One.From]   = Moved;
    if (KindOf(Moved) == King) Kings[Turn] = One.From;
    if (One.Is(Move::InPassing)) Board[static_cast<std::size_t>(One.To + (Turn == White ? -16 : 16))] = Kept.Taken;
    else Board[One.To] = Kept.Taken;
    if (One.Is(Move::Castle)) {
        const bool Short = FileOf(One.To) == 6;
        const int  Rank  = RankOf(One.To);
        Board[static_cast<std::size_t>(SquareAt(Short ? 7 : 0, Rank))] = Board[static_cast<std::size_t>(SquareAt(Short ? 5 : 3, Rank))];
        Board[static_cast<std::size_t>(SquareAt(Short ? 5 : 3, Rank))] = 0;
    }
    Castling  = Kept.Castling;
    InPassing = Kept.InPassing;
    Fifty     = Kept.Fifty;
    Hash      = Kept.Hash;
}

void Position::Pass()
{
    History.push_back(Was{Move{}, 0, Castling, InPassing, Fifty, Hash});
    if (InPassing >= 0) Hash ^= Zobrist.Files[static_cast<std::size_t>(FileOf(InPassing))];
    InPassing = -1;
    Turn      = Other(Turn);
    Hash ^= Zobrist.BlackToMove;
}

void Position::Unpass()
{
    const Was Kept = History.back();
    History.pop_back();
    Turn      = Other(Turn);
    InPassing = Kept.InPassing;
    Hash      = Kept.Hash;
}

bool Position::HasPieces(Side Of) const
{
    for (Square At_ = 0; At_ < 128; At_++) {
        const Piece One = OnBoard(At_) ? Board[static_cast<std::size_t>(At_)] : 0;
        if (One != 0 && SideOf(One) == Of && KindOf(One) != Pawn && KindOf(One) != King) return true;
    }
    return false;
}

bool Position::Repeated(int Times) const
{
    int Seen = 0;
    /* Only the side to move's own positions, back to the last capture or
     * pawn move: nothing before it can stand again. */
    const int Back = static_cast<int>(History.size());
    for (int Ago = 2; Ago <= Fifty && Ago <= Back; Ago += 2)
        if (History[static_cast<std::size_t>(Back - Ago)].Hash == Hash && ++Seen >= Times) return true;
    return false;
}

bool Position::TooLittle() const
{
    int Minors = 0, BishopColours = 0, Bishops = 0;
    for (Square At_ = 0; At_ < 128; At_++) {
        if (!OnBoard(At_)) continue;
        const Piece One = Board[static_cast<std::size_t>(At_)];
        switch (KindOf(One)) {
        case Pawn:
        case Rook:
        case Queen: return false;
        case Knight: Minors++; break;
        case Bishop:
            Minors++;
            Bishops++;
            BishopColours |= 1 << ((FileOf(At_) + RankOf(At_)) & 1);
            break;
        default: break;
        }
    }
    /* A king alone against a king and one piece, or bishops all on one
     * colour of square, can never be mated. */
    return Minors <= 1 || (Bishops == Minors && BishopColours != 3);
}

Outcome Position::Result() const
{
    MoveList Legal;
    Moves(Legal);
    if (Legal.Count == 0) return InCheck() ? Outcome::Checkmate : Outcome::Stalemate;
    if (Fifty >= 100) return Outcome::FiftyMoves;
    if (Repeated(2)) return Outcome::Repetition;
    if (TooLittle()) return Outcome::Material;
    return Outcome::Playing;
}

Move Position::Find(Square From, Square To, Kind Promote) const
{
    MoveList Legal;
    Moves(Legal);
    for (const Move One : Legal)
        if (One.From == From && One.To == To && One.Promote == Promote) return One;
    return {};
}

std::string Position::Written(Move One) const
{
    std::string Made;
    const Piece Mover = Board[One.From];
    const Kind  Is    = KindOf(Mover);
    if (One.Is(Move::Castle)) Made = FileOf(One.To) == 6 ? "O-O" : "O-O-O";
    else if (Is == Pawn) {
        if (One.Is(Move::Capture)) Made = {static_cast<char>('a' + FileOf(One.From)), 'x'};
        Made += SquareName(One.To);
        if (One.Promote != NoKind) Made += {'=', Letters[One.Promote]};
    } else {
        Made = Letters[Is];
        /* Another piece of the kind that could go there too: the file says
         * which, or the rank when the file does not, or both. */
        MoveList Legal;
        Moves(Legal);
        bool Twin = false, SameFile = false, SameRank = false;
        for (const Move Other_ : Legal)
            if (Other_.To == One.To && Other_.From != One.From && Board[Other_.From] == Mover) {
                Twin = true;
                SameFile |= FileOf(Other_.From) == FileOf(One.From);
                SameRank |= RankOf(Other_.From) == RankOf(One.From);
            }
        if (Twin && (!SameFile || SameRank)) Made += static_cast<char>('a' + FileOf(One.From));
        if (Twin && SameFile) Made += static_cast<char>('1' + RankOf(One.From));
        if (One.Is(Move::Capture)) Made += 'x';
        Made += SquareName(One.To);
    }
    Position After = *this;
    if (After.Play(One) && After.InCheck()) {
        MoveList Answers;
        After.Moves(Answers);
        Made += Answers.Count == 0 ? '#' : '+';
    }
    return Made;
}

}  // namespace chess
