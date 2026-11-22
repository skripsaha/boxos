/* position.h — the rules of chess: where the pieces stand, whose move it is,
 * the castlings still to be had, the square a pawn may be taken on in
 * passing, the moves since the last capture or pawn move; every legal move
 * from here, a move played and taken back, check, mate and every draw the
 * rules know — stalemate, the same position a third time, fifty moves, too
 * little left to mate — and a move as a person writes it (e4, Nxf7+, O-O,
 * e8=Q#). No screen, nothing of the machine: the engine searches these.
 *
 * The board is 0x88: a square is its rank times 16 and its file, so a step
 * off the board shows in the bits 0x88 and a move is checked with one AND. */
#ifndef CHESS_POSITION_H
#define CHESS_POSITION_H

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace chess {

enum Side : std::uint8_t { White = 0, Black = 1 };

inline constexpr Side Other(Side Of) noexcept { return static_cast<Side>(Of ^ 1); }

enum Kind : std::uint8_t { NoKind = 0, Pawn, Knight, Bishop, Rook, Queen, King };

/* A piece: its kind, and 8 for black; 0 an empty square. */
using Piece = std::uint8_t;

inline constexpr Piece PieceOf(Side Of, Kind Is) noexcept { return static_cast<Piece>(Is | (Of << 3)); }
inline constexpr Kind  KindOf(Piece Of) noexcept { return static_cast<Kind>(Of & 7); }
inline constexpr Side  SideOf(Piece Of) noexcept { return static_cast<Side>(Of >> 3); }

using Square = int;

inline constexpr Square SquareAt(int File, int Rank) noexcept { return Rank * 16 + File; }
inline constexpr int    FileOf(Square Of) noexcept { return Of & 7; }
inline constexpr int    RankOf(Square Of) noexcept { return Of >> 4; }
inline constexpr bool   OnBoard(int Of) noexcept { return (Of & 0x88) == 0; }
/* 0 to 63, a1 to h8, rank by rank. */
inline constexpr int    Index(Square Of) noexcept { return RankOf(Of) * 8 + FileOf(Of); }

struct Move {
    enum Flag : std::uint8_t { Capture = 1, InPassing = 2, Castle = 4, Double = 8 };
    std::uint8_t From = 0, To = 0;
    Kind         Promote = NoKind;
    std::uint8_t Flags   = 0;

    bool Is(Flag Of) const noexcept { return (Flags & Of) != 0; }
    explicit operator bool() const noexcept { return From != To; }
    bool operator==(const Move &) const = default;
};

/* As many moves as any position has, and more. */
struct MoveList {
    std::array<Move, 256> At;
    int                   Count = 0;
    void Add(Move One) noexcept { At[static_cast<std::size_t>(Count++)] = One; }
    const Move *begin() const noexcept { return At.data(); }
    const Move *end() const noexcept { return At.data() + Count; }
};

enum class Outcome : std::uint8_t { Playing, Checkmate, Stalemate, Repetition, FiftyMoves, Material };

class Position {
public:
    /* The pieces as a game begins. */
    Position();

    Piece  At(Square Of) const noexcept { return Board[static_cast<std::size_t>(Of)]; }
    Side   ToMove() const noexcept { return Turn; }
    Square KingOf(Side Of) const noexcept { return Kings[Of]; }
    int    SinceCaptureOrPawn() const noexcept { return Fifty; }
    std::uint64_t Key() const noexcept { return Hash; }
    /* Half-moves played since the start. */
    int    Played() const noexcept { return static_cast<int>(History.size()); }
    /* The last move played, if any. */
    Move   Last() const noexcept { return History.empty() ? Move{} : History.back().Played; }

    /* Every move the side to move may make; Captures, only captures and
     * promotions. Pseudo-legal: one may leave its own king in check, and
     * Play refuses it. */
    void Candidates(MoveList &Into, bool Captures = false) const;
    /* Every legal move. */
    void Moves(MoveList &Into) const;
    /* The move played — false, and the board as it was, when it leaves the
     * mover's king in check. */
    bool Play(Move One);
    void Undo();
    /* Nobody's move but the turn passed: for the engine, which asks whether a
     * position is good even if the side to move does nothing. */
    void Pass();
    void Unpass();

    bool Attacked(Square Of, Side By) const;
    bool InCheck() const { return Attacked(Kings[Turn], Other(Turn)); }
    /* The side to move has more than pawns and its king. */
    bool HasPieces(Side Of) const;
    /* The position has stood before since the last capture or pawn move:
     * Times more times than now. */
    bool Repeated(int Times) const;
    /* Neither side has enough left to mate. */
    bool TooLittle() const;
    Outcome Result() const;

    /* A legal move as a person writes it, said before it is played. */
    std::string Written(Move One) const;
    /* The legal move from From to To, promoting to Promote; none when there
     * is no such move. */
    Move Find(Square From, Square To, Kind Promote = NoKind) const;

private:
    struct Was {
        Move          Played;
        Piece         Taken;
        std::uint8_t  Castling;
        int           InPassing;
        int           Fifty;
        std::uint64_t Hash;
    };

    void Put(Square Of, Piece One);
    void Lift(Square Of);
    void Pawns(MoveList &Into, Square From, bool Captures) const;
    void Castlings(MoveList &Into) const;
    /* Whether a pawn of the side to move could take on InPassing: a square
     * none can take on is no square to take on. */
    bool CanTakeInPassing() const;

    std::array<Piece, 128>  Board{};
    Side                    Turn      = White;
    std::uint8_t            Castling  = 15;   // 1 white short, 2 white long, 4 black short, 8 black long
    int                     InPassing = -1;   // the square behind a pawn that has just moved two
    int                     Fifty     = 0;
    std::array<Square, 2>   Kings{};
    std::uint64_t           Hash = 0;
    std::vector<Was>        History;
};

/* "e4": a square's name. */
std::string SquareName(Square Of);

}  // namespace chess

#endif
