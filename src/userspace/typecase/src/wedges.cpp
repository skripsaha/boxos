/* wedges.cpp — the wedges of Symbols for Legacy Computing. */

#include "wedges.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "typecase/outline.h"

namespace typecase {

namespace {

constexpr char32_t FirstWedge = 0x1FB3C, FirstComplement = 0x1FB52, FirstThreeQuarters = 0x1FB68,
                   FirstQuarter = 0x1FB6C, LastWedge = 0x1FB6F;

/* The points a wedge's sides run between: the first ten its edge, in order
 * round it from the top left, the last the middle of the cell, where the
 * triangles meet. */
enum Spot : std::uint8_t {
    UpperLeft, UpperCentre, UpperRight, UpperMiddleRight, LowerMiddleRight,
    LowerRight, LowerCentre, LowerLeft, LowerMiddleLeft, UpperMiddleLeft, Middle,
};
constexpr int Round = 10;

/* The part of the cell the diagonal From..To cuts off on the side of Corner.
 * Read off the characters' Unicode names, U+1FB3C onward; the 22 from
 * U+1FB52 are their complements — the same diagonals, the opposite corner. */
struct Cut {
    Spot Corner, From, To;
};

constexpr Cut Cuts[22] = {
    {LowerLeft, LowerMiddleLeft, LowerCentre},       /* U+1FB3C */
    {LowerLeft, LowerMiddleLeft, LowerRight},        /* U+1FB3D */
    {LowerLeft, UpperMiddleLeft, LowerCentre},       /* U+1FB3E */
    {LowerLeft, UpperMiddleLeft, LowerRight},        /* U+1FB3F */
    {LowerLeft, UpperLeft, LowerCentre},             /* U+1FB40 */
    {LowerRight, UpperMiddleLeft, UpperCentre},      /* U+1FB41 */
    {LowerRight, UpperMiddleLeft, UpperRight},       /* U+1FB42 */
    {LowerRight, LowerMiddleLeft, UpperCentre},      /* U+1FB43 */
    {LowerRight, LowerMiddleLeft, UpperRight},       /* U+1FB44 */
    {LowerRight, LowerLeft, UpperCentre},            /* U+1FB45 */
    {LowerRight, LowerMiddleLeft, UpperMiddleRight}, /* U+1FB46 */
    {LowerRight, LowerCentre, LowerMiddleRight},     /* U+1FB47 */
    {LowerRight, LowerLeft, LowerMiddleRight},       /* U+1FB48 */
    {LowerRight, LowerCentre, UpperMiddleRight},     /* U+1FB49 */
    {LowerRight, LowerLeft, UpperMiddleRight},       /* U+1FB4A */
    {LowerRight, LowerCentre, UpperRight},           /* U+1FB4B */
    {LowerLeft, UpperCentre, UpperMiddleRight},      /* U+1FB4C */
    {LowerLeft, UpperLeft, UpperMiddleRight},        /* U+1FB4D */
    {LowerLeft, UpperCentre, LowerMiddleRight},      /* U+1FB4E */
    {LowerLeft, UpperLeft, LowerMiddleRight},        /* U+1FB4F */
    {LowerLeft, UpperCentre, LowerRight},            /* U+1FB50 */
    {LowerLeft, UpperMiddleLeft, LowerMiddleRight},  /* U+1FB51 */
};

/* The corners in order round the cell: the triangles from U+1FB6C stand on
 * its left, upper, right and lower side — between corners 3 and 0, 0 and 1,
 * 1 and 2, 2 and 3 — and the three quarters from U+1FB68 are the cell
 * without them, in the same order. */
constexpr Spot Corners[4] = {UpperLeft, UpperRight, LowerRight, LowerLeft};

Spot Opposite(Spot Corner)
{
    switch (Corner) {
    case UpperLeft:  return LowerRight;
    case UpperRight: return LowerLeft;
    case LowerRight: return UpperLeft;
    default:         return UpperRight;
    }
}

/* The edge of the cell from one of its spots round to another. */
std::vector<Spot> Along(Spot From, Spot To)
{
    std::vector<Spot> Way;
    for (int At = From;; At = (At + 1) % Round) {
        Way.push_back(static_cast<Spot>(At));
        if (At == To) return Way;
    }
}

/* The wedge as the spots of its outline, in order. */
std::vector<Spot> ShapeOf(char32_t Character)
{
    if (Character >= FirstQuarter) {
        const int Side = static_cast<int>(Character - FirstQuarter);
        return {Corners[(Side + 3) % 4], Corners[Side], Middle};
    }
    if (Character >= FirstThreeQuarters) {
        const int Side = static_cast<int>(Character - FirstThreeQuarters);
        return {Corners[Side], Corners[(Side + 1) % 4], Corners[(Side + 2) % 4], Corners[(Side + 3) % 4], Middle};
    }
    const bool Complement = Character >= FirstComplement;
    const Cut &Of          = Cuts[Character - (Complement ? FirstComplement : FirstWedge)];
    const Spot Corner      = Complement ? Opposite(Of.Corner) : Of.Corner;
    std::vector<Spot> Part = Along(Of.From, Of.To);
    if (std::find(Part.begin(), Part.end(), Corner) == Part.end()) Part = Along(Of.To, Of.From);
    return Part;
}

/* Where a spot lies in a cell Width by Height: the middles and thirds on the
 * pixels the mosaics' edges take. */
Point Where(Spot Of, std::int32_t Width, std::int32_t Height)
{
    const auto  At     = [](std::int32_t Value) { return static_cast<float>(Value); };
    const float Centre = At(Edge(1, 2, Width)), Right = At(Width);
    const float Upper = At(Edge(1, 3, Height)), Lower = At(Edge(2, 3, Height)), Foot = At(Height);
    switch (Of) {
    case UpperLeft:        return {0, 0};
    case UpperCentre:      return {Centre, 0};
    case UpperRight:       return {Right, 0};
    case UpperMiddleRight: return {Right, Upper};
    case LowerMiddleRight: return {Right, Lower};
    case LowerRight:       return {Right, Foot};
    case LowerCentre:      return {Centre, Foot};
    case LowerLeft:        return {0, Foot};
    case LowerMiddleLeft:  return {0, Lower};
    case UpperMiddleLeft:  return {0, Upper};
    default:               return {Centre, At(Edge(1, 2, Height))};
    }
}

}  // namespace

bool IsWedge(char32_t Character) noexcept
{
    return Character >= FirstWedge && Character <= LastWedge;
}

void DrawWedge(char32_t Character, CellInk &Ink, Rasterizer &Pen)
{
    Outline Shape;
    for (const Spot Each : ShapeOf(Character)) {
        Shape.Steps.push_back(Shape.Points.empty() ? Step::Move : Step::Line);
        Shape.Points.push_back(Where(Each, Ink.Width, Ink.Height));
    }
    Ink.Smooth(Shape, Pen);
}

}  // namespace typecase
