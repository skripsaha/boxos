/* The engine of difference: the fewest lines taken out and put in.
 *
 * Lines are first made into numbers: a hash puts equal lines side by side and
 * their bytes are then compared, so two lines get one number exactly when
 * they are the same. A line whose number the other file never has cannot be
 * in any common run, so it is marked at once; for two files that share
 * little this takes away nearly all the work, and the answer is the same.
 * What is left goes through Myers' O(ND) middle snake, which finds a point
 * on a best path from both ends and divides there, so memory is the lines
 * and two arrays of their size however many differences there are. The search
 * has patience: two files that differ in more than that many lines of one
 * stretch are not worth the time of the least answer, and the furthest point
 * reached stands in for the middle; the answer still turns one file into the
 * other, with perhaps a few lines more changed than the least.
 */

#include <algorithm>
#include <cstring>

#include "difference.h"

namespace {

using LineId = std::uint32_t;

struct HashedLine {
    std::uint64_t Hash;
    std::uint32_t Index;   /* in the first file's lines then the second's */
};

std::uint64_t HashOf(std::string_view Line)
{
    std::uint64_t Hash = 0xcbf29ce484222325ull ^ Line.size();
    const char   *At   = Line.data();
    const char   *End  = At + Line.size();
    while (End - At >= 8) {
        std::uint64_t Word;
        std::memcpy(&Word, At, 8);
        Hash = (Hash ^ Word) * 0x9e3779b97f4a7c15ull;
        Hash ^= Hash >> 29;
        At += 8;
    }
    while (At < End) Hash = (Hash ^ static_cast<std::uint8_t>(*At++)) * 0x100000001b3ull;
    return Hash ^ (Hash >> 32);
}

struct Numbered {
    std::vector<LineId> First;
    std::vector<LineId> Second;
    std::size_t         Kinds = 0;
};

Numbered Number(std::span<const std::string_view> First, std::span<const std::string_view> Second)
{
    auto LineAt = [&](std::uint32_t Index) { return Index < First.size() ? First[Index] : Second[Index - First.size()]; };

    std::vector<HashedLine> Hashed;
    Hashed.reserve(First.size() + Second.size());
    for (std::size_t I = 0; I < First.size() + Second.size(); I++)
        Hashed.push_back({HashOf(LineAt(static_cast<std::uint32_t>(I))), static_cast<std::uint32_t>(I)});
    std::sort(Hashed.begin(), Hashed.end(), [](const HashedLine &A, const HashedLine &B) {
        return A.Hash != B.Hash ? A.Hash < B.Hash : A.Index < B.Index;
    });

    Numbered Result;
    Result.First.resize(First.size());
    Result.Second.resize(Second.size());

    struct Kind {
        std::uint32_t Example;
        LineId        Id;
    };
    std::vector<Kind> SameHash;
    LineId            Next = 0;
    for (std::size_t Begin = 0; Begin < Hashed.size();) {
        std::size_t End = Begin;
        while (End < Hashed.size() && Hashed[End].Hash == Hashed[Begin].Hash) End++;

        SameHash.clear();
        for (std::size_t I = Begin; I < End; I++) {
            std::uint32_t Index = Hashed[I].Index;
            LineId        Id    = Next;
            bool          Known = false;
            for (const Kind &Seen : SameHash) {
                if (LineAt(Seen.Example) == LineAt(Index)) {
                    Id    = Seen.Id;
                    Known = true;
                    break;
                }
            }
            if (!Known) {
                SameHash.push_back({Index, Next});
                Next++;
            }
            if (Index < First.size()) Result.First[Index] = Id;
            else                      Result.Second[Index - First.size()] = Id;
        }
        Begin = End;
    }
    Result.Kinds = Next;
    return Result;
}

/* The lines worth comparing in one file, and where each stands in the file. */
struct Kept {
    std::vector<LineId>      Ids;
    std::vector<std::size_t> Origin;
};

Kept KeepSharedWith(const std::vector<LineId> &Lines, const std::vector<std::uint8_t> &Other)
{
    Kept Result;
    for (std::size_t I = 0; I < Lines.size(); I++) {
        if (!Other[Lines[I]]) continue;
        Result.Ids.push_back(Lines[I]);
        Result.Origin.push_back(I);
    }
    return Result;
}

std::vector<std::uint8_t> Seen(const std::vector<LineId> &Lines, std::size_t Kinds)
{
    std::vector<std::uint8_t> Present(Kinds, 0);
    for (LineId Id : Lines) Present[Id] = 1;
    return Present;
}

class Bisector {
public:
    Bisector(const Kept &First, const Kept &Second, DifferenceMarks &Marks, std::size_t Patience)
        : First(First), Second(Second), Marks(Marks), Patience(static_cast<std::ptrdiff_t>(Patience)),
          Forward(First.Ids.size() + Second.Ids.size() + 3), Backward(First.Ids.size() + Second.Ids.size() + 3)
    {
    }

    void Compare(std::size_t FirstBegin, std::size_t FirstEnd, std::size_t SecondBegin, std::size_t SecondEnd);

private:
    struct Point {
        std::ptrdiff_t X;
        std::ptrdiff_t Y;
    };

    bool Middle(std::size_t FirstBegin, std::size_t FirstEnd, std::size_t SecondBegin, std::size_t SecondEnd, Point &Out);
    bool Furthest(std::ptrdiff_t N, std::ptrdiff_t M, std::ptrdiff_t Offset, std::ptrdiff_t Length, Point &Out) const;
    void TakeOut(std::size_t Begin, std::size_t End);
    void PutIn(std::size_t Begin, std::size_t End);

    const Kept       &First;
    const Kept       &Second;
    DifferenceMarks  &Marks;
    std::ptrdiff_t    Patience;
    std::vector<std::int32_t> Forward;
    std::vector<std::int32_t> Backward;
};

void Bisector::TakeOut(std::size_t Begin, std::size_t End)
{
    for (std::size_t I = Begin; I < End; I++) Marks.TakenOut[First.Origin[I]] = 1;
}

void Bisector::PutIn(std::size_t Begin, std::size_t End)
{
    for (std::size_t I = Begin; I < End; I++) Marks.PutIn[Second.Origin[I]] = 1;
}

void Bisector::Compare(std::size_t FirstBegin, std::size_t FirstEnd, std::size_t SecondBegin, std::size_t SecondEnd)
{
    while (FirstBegin < FirstEnd && SecondBegin < SecondEnd && First.Ids[FirstBegin] == Second.Ids[SecondBegin]) {
        FirstBegin++;
        SecondBegin++;
    }
    while (FirstBegin < FirstEnd && SecondBegin < SecondEnd && First.Ids[FirstEnd - 1] == Second.Ids[SecondEnd - 1]) {
        FirstEnd--;
        SecondEnd--;
    }
    if (FirstBegin == FirstEnd) {
        PutIn(SecondBegin, SecondEnd);
        return;
    }
    if (SecondBegin == SecondEnd) {
        TakeOut(FirstBegin, FirstEnd);
        return;
    }

    Point Split;
    if (!Middle(FirstBegin, FirstEnd, SecondBegin, SecondEnd, Split)) {
        TakeOut(FirstBegin, FirstEnd);
        PutIn(SecondBegin, SecondEnd);
        return;
    }
    std::size_t FirstSplit  = FirstBegin + static_cast<std::size_t>(Split.X);
    std::size_t SecondSplit = SecondBegin + static_cast<std::size_t>(Split.Y);
    Compare(FirstBegin, FirstSplit, SecondBegin, SecondSplit);
    Compare(FirstSplit, FirstEnd, SecondSplit, SecondEnd);
}

/* Of all the points the two searches reached, the one that has come furthest
 * from its own corner, if it is strictly inside the grid. */
bool Bisector::Furthest(std::ptrdiff_t N, std::ptrdiff_t M, std::ptrdiff_t Offset, std::ptrdiff_t Length, Point &Out) const
{
    std::ptrdiff_t BestForward = 0, BestBackward = 0;
    Point          ForwardAt{0, 0}, BackwardAt{0, 0};
    for (std::ptrdiff_t At = 0; At < Length; At++) {
        std::ptrdiff_t K = At - Offset;
        std::ptrdiff_t X = Forward[At];
        if (X >= 0 && X <= N && X - K >= 0 && X - K <= M && X + (X - K) > BestForward) {
            BestForward = X + (X - K);
            ForwardAt   = {X, X - K};
        }
        X = Backward[At];
        if (X >= 0 && X <= N && X - K >= 0 && X - K <= M && X + (X - K) > BestBackward) {
            BestBackward = X + (X - K);
            BackwardAt   = {N - X, M - (X - K)};
        }
    }
    Out = BestForward >= BestBackward ? ForwardAt : BackwardAt;
    return Out.X + Out.Y > 0 && Out.X + Out.Y < N + M;
}

/* Paths grow from the start forward and from the end backward, one more edit
 * each turn, until one meets the other. Diagonal K of the forward path is
 * X - Y; the backward path, which counts from the far corner, meets it on
 * diagonal Delta - K. A path that leaves the grid is dropped from the
 * diagonals tried next (the Start and End that move in by two). */
bool Bisector::Middle(std::size_t FirstBegin, std::size_t FirstEnd, std::size_t SecondBegin, std::size_t SecondEnd,
                      Point &Out)
{
    const std::ptrdiff_t N      = static_cast<std::ptrdiff_t>(FirstEnd - FirstBegin);
    const std::ptrdiff_t M      = static_cast<std::ptrdiff_t>(SecondEnd - SecondBegin);
    const std::ptrdiff_t MaxD   = (N + M + 1) / 2;
    const std::ptrdiff_t Offset = MaxD;
    const std::ptrdiff_t Length = 2 * MaxD + 2;
    const std::ptrdiff_t Delta  = N - M;
    const bool           Odd    = (Delta % 2) != 0;

    std::fill(Forward.begin(), Forward.begin() + Length, -1);
    std::fill(Backward.begin(), Backward.begin() + Length, -1);
    Forward[Offset + 1]  = 0;
    Backward[Offset + 1] = 0;

    const LineId *A = First.Ids.data() + FirstBegin;
    const LineId *B = Second.Ids.data() + SecondBegin;

    std::ptrdiff_t ForwardStart = 0, ForwardEnd = 0, BackwardStart = 0, BackwardEnd = 0;
    for (std::ptrdiff_t D = 0; D < MaxD; D++) {
        if (D > Patience) return Furthest(N, M, Offset, Length, Out);
        for (std::ptrdiff_t K = -D + ForwardStart; K <= D - ForwardEnd; K += 2) {
            std::ptrdiff_t At = Offset + K;
            std::ptrdiff_t X  = (K == -D || (K != D && Forward[At - 1] < Forward[At + 1])) ? Forward[At + 1] : Forward[At - 1] + 1;
            std::ptrdiff_t Y  = X - K;
            while (X < N && Y < M && A[X] == B[Y]) {
                X++;
                Y++;
            }
            Forward[At] = static_cast<std::int32_t>(X);
            if (X > N) {
                ForwardEnd += 2;
            } else if (Y > M) {
                ForwardStart += 2;
            } else if (Odd) {
                std::ptrdiff_t Meet = Offset + Delta - K;
                if (Meet >= 0 && Meet < Length && Backward[Meet] != -1 && X >= N - Backward[Meet]) {
                    Out = {X, Y};
                    return true;
                }
            }
        }

        for (std::ptrdiff_t K = -D + BackwardStart; K <= D - BackwardEnd; K += 2) {
            std::ptrdiff_t At = Offset + K;
            std::ptrdiff_t X  = (K == -D || (K != D && Backward[At - 1] < Backward[At + 1])) ? Backward[At + 1] : Backward[At - 1] + 1;
            std::ptrdiff_t Y  = X - K;
            while (X < N && Y < M && A[N - 1 - X] == B[M - 1 - Y]) {
                X++;
                Y++;
            }
            Backward[At] = static_cast<std::int32_t>(X);
            if (X > N) {
                BackwardEnd += 2;
            } else if (Y > M) {
                BackwardStart += 2;
            } else if (!Odd) {
                std::ptrdiff_t Meet = Offset + Delta - K;
                if (Meet >= 0 && Meet < Length && Forward[Meet] != -1) {
                    std::ptrdiff_t ForwardX = Forward[Meet];
                    if (ForwardX >= N - X) {
                        Out = {ForwardX, ForwardX - (Meet - Offset)};
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

}

DifferenceMarks DifferenceCompare(std::span<const std::string_view> First, std::span<const std::string_view> Second,
                                  std::size_t Patience)
{
    DifferenceMarks Marks;
    Marks.TakenOut.assign(First.size(), 1);
    Marks.PutIn.assign(Second.size(), 1);

    Numbered Lines = Number(First, Second);
    Kept     FirstKept  = KeepSharedWith(Lines.First, Seen(Lines.Second, Lines.Kinds));
    Kept     SecondKept = KeepSharedWith(Lines.Second, Seen(Lines.First, Lines.Kinds));
    for (std::size_t Origin : FirstKept.Origin) Marks.TakenOut[Origin] = 0;
    for (std::size_t Origin : SecondKept.Origin) Marks.PutIn[Origin] = 0;

    Bisector Bisect(FirstKept, SecondKept, Marks, Patience);
    Bisect.Compare(0, FirstKept.Ids.size(), 0, SecondKept.Ids.size());
    return Marks;
}
