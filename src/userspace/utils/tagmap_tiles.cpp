/* tagmap_tiles.cpp — tiles laid and the way between them (tagmap.h). */

#include <algorithm>
#include <cmath>
#include <format>

#include "tagmap.h"

namespace {

/* The least a tile of its own is: three rows by ten columns — two rows of
 * words in it past the half-cell gap at its edges — in half-row units. */
constexpr double LeastArea = 3 * 2 * 10;
constexpr std::size_t MostTiles = 64;

double Floored(std::uint64_t Weight)
{
    return static_cast<double>(std::max<std::uint64_t>(Weight, 1));
}

/* A place on the glass in columns and half rows: a cell is two high. */
struct Room {
    double X, Y, W, H;
};

/* How far from square the worst of a row of tiles would be, laid along a
 * side `Side` long, the row's areas from `Most` down to `Least` summing to
 * `Sum`. */
double Worst(double Most, double Least, double Sum, double Side)
{
    double Sum2 = Sum * Sum, Side2 = Side * Side;
    return std::max(Side2 * Most / Sum2, Sum2 / (Side2 * Least));
}

}

std::size_t TagmapFit(const std::vector<std::uint64_t> &Weights, int High, int Wide, std::uint64_t &MoreWeight)
{
    MoreWeight = 0;
    std::size_t Count = Weights.size();
    if (Count == 0) return 0;
    double Area = static_cast<double>(High) * 2.0 * Wide;

    std::vector<double> Prefix(Count + 1, 0.0);
    for (std::size_t At = 0; At < Count; At++) Prefix[At + 1] = Prefix[At] + Floored(Weights[At]);

    for (std::size_t Shown = std::min(Count, MostTiles); Shown >= 1; Shown--) {
        double Own = Prefix[Shown], Left = Prefix[Count] - Own, More = 0;
        if (Shown < Count) {
            double Least = Area > LeastArea ? std::ceil(LeastArea * Own / (Area - LeastArea)) : Own;
            More = std::clamp(Left, Least, std::max(Least, Own / 2));
        }
        if (Floored(Weights[Shown - 1]) * Area >= LeastArea * (Own + More) || Shown == 1) {
            MoreWeight = Shown < Count ? static_cast<std::uint64_t>(std::max(1.0, More)) : 0;
            return Shown;
        }
    }
    return 0;
}

/* Each row of the squarified layout takes as many of the next tiles as keep
 * its worst tile nearer square, laid across the room's short side; the room
 * left is what the row did not take. A tile's edges are rounded to cells by
 * where they are, not by its size, so neighbours share their edge exactly. */
std::vector<TagmapTile> TagmapLay(const std::vector<std::uint64_t> &Weights, int Row, int Col, int High, int Wide)
{
    std::vector<TagmapTile> Tiles;
    if (Weights.empty() || High <= 0 || Wide <= 0) return Tiles;

    double Total = 0;
    for (std::uint64_t Weight : Weights) Total += Floored(Weight);
    double              Scale = static_cast<double>(High) * 2.0 * Wide / Total;
    std::vector<double> Areas;
    for (std::uint64_t Weight : Weights) Areas.push_back(Floored(Weight) * Scale);

    auto Place = [&](const Room &Part, std::size_t Index) {
        int Left   = std::clamp(static_cast<int>(std::lround(Part.X)), 0, Wide);
        int Right  = std::clamp(static_cast<int>(std::lround(Part.X + Part.W)), 0, Wide);
        int Top    = std::clamp(static_cast<int>(std::lround(Part.Y / 2)), 0, High);
        int Bottom = std::clamp(static_cast<int>(std::lround((Part.Y + Part.H) / 2)), 0, High);
        if (Right > Left && Bottom > Top) Tiles.push_back({Row + Top, Col + Left, Bottom - Top, Right - Left, Index});
    };

    Room        Free{0, 0, static_cast<double>(Wide), static_cast<double>(High) * 2.0};
    std::size_t At = 0;
    while (At < Areas.size()) {
        double      Side = std::min(Free.W, Free.H);
        std::size_t End  = At + 1;
        double      Sum  = Areas[At];
        double      Bad  = Worst(Areas[At], Areas[At], Sum, Side);
        while (End < Areas.size()) {
            double Wider = Worst(Areas[At], Areas[End], Sum + Areas[End], Side);
            if (Wider > Bad) break;
            Sum += Areas[End++];
            Bad = Wider;
        }

        bool   Column = Free.W >= Free.H;
        double Thick  = Sum / (Column ? Free.H : Free.W);
        if (End == Areas.size()) Thick = Column ? Free.W : Free.H;
        double Along  = Column ? Free.Y : Free.X;
        for (std::size_t One = At; One < End; One++) {
            double Length = Areas[One] / Sum * (Column ? Free.H : Free.W);
            if (One + 1 == End) Length = (Column ? Free.Y + Free.H : Free.X + Free.W) - Along;
            Place(Column ? Room{Free.X, Along, Thick, Length} : Room{Along, Free.Y, Length, Thick}, One);
            Along += Length;
        }
        if (Column) {
            Free.X += Thick;
            Free.W -= Thick;
        } else {
            Free.Y += Thick;
            Free.H -= Thick;
        }
        At = End;
    }
    return Tiles;
}

std::size_t TagmapStep(const std::vector<TagmapTile> &Tiles, std::size_t From, TagmapWay Way)
{
    if (From >= Tiles.size()) return From;
    const TagmapTile &Here    = Tiles[From];
    bool              Across  = Way == TagmapWay::Left || Way == TagmapWay::Right;
    bool              Forward = Way == TagmapWay::Right || Way == TagmapWay::Down;

    /* The tile's span on an axis, in half-row units: [Begin, End). */
    auto Span = [](const TagmapTile &Tile, bool Columns, int &Begin, int &End) {
        Begin = Columns ? Tile.Col : Tile.Row * 2;
        End   = Columns ? Tile.Col + Tile.Wide : (Tile.Row + Tile.High) * 2;
    };
    int HereBegin, HereEnd, HereSideBegin, HereSideEnd;
    Span(Here, Across, HereBegin, HereEnd);
    Span(Here, !Across, HereSideBegin, HereSideEnd);

    std::size_t Best = From;
    long        BestScore = 0, BestAslant = 0;
    for (std::size_t At = 0; At < Tiles.size(); At++) {
        int Begin, End, SideBegin, SideEnd;
        Span(Tiles[At], Across, Begin, End);
        Span(Tiles[At], !Across, SideBegin, SideEnd);
        int Gap = Forward ? Begin - HereEnd : HereBegin - End;
        if (At == From || Gap < 0) continue;
        int  Apart  = std::max(0, std::max(SideBegin, HereSideBegin) - std::min(SideEnd, HereSideEnd));
        long Score  = Gap + 2L * Apart;
        long Aslant = std::labs(static_cast<long>(SideBegin + SideEnd) - (HereSideBegin + HereSideEnd));
        if (Best == From || Score < BestScore || (Score == BestScore && Aslant < BestAslant)) {
            Best       = At;
            BestScore  = Score;
            BestAslant = Aslant;
        }
    }
    return Best;
}

std::string TagmapSize(std::uint64_t Bytes)
{
    static const char *const Units[] = {"KiB", "MiB", "GiB", "TiB"};
    if (Bytes < 1024) return std::format("{} B", Bytes);
    double Size = static_cast<double>(Bytes) / 1024.0;
    int    Unit = 0;
    while (Size >= 1024.0 && Unit < 3) {
        Size /= 1024.0;
        Unit++;
    }
    return std::format("{:.1f} {}", Size, Units[Unit]);
}
