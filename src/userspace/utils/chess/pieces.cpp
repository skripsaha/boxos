/* pieces.cpp — the pieces' shapes. See pieces.h. Each part is worked out in
 * the 100-unit box, its foot standing on 90, and outlined as it is laid. */

#include "pieces.h"

#include "look.h"

namespace chess {

namespace {

using painter::core::Color;
using painter::core::Path;
using painter::core::PointF;

/* A part: its outline, and whether it is filled (a body) or only drawn (a
 * line on one). */
struct Part {
    Path Shape;
    bool Line = false;
};

class Drawing {
public:
    explicit Drawing(float Size) : U(Size / 100.0f) {}

    PointF At(float X, float Y) const { return {X * U, Y * U}; }
    Path  &Body()
    {
        Parts.push_back(Part{});
        return Parts.back().Shape;
    }
    Path &Line()
    {
        Parts.push_back(Part{Path{}, true});
        return Parts.back().Shape;
    }
    void Round(float X, float Y, float R) { Body().AddCircle(At(X, Y), R * U); }
    void Oval(float X, float Y, float RX, float RY) { Body().AddEllipse(At(X, Y), RX * U, RY * U); }

    /* The foot every piece stands on: a broad turned base and a ring on it. */
    void Foot()
    {
        Body().AddRoundedRect({20 * U, 81 * U, 60 * U, 9 * U}, painter::core::Corners::All(3.5f * U));
        Body().AddRoundedRect({26 * U, 76 * U, 48 * U, 7 * U}, painter::core::Corners::All(3 * U));
    }
    /* A body swelling from a waist at Waist, Narrow either side of the
     * middle, down to the ring, Wide either side. */
    void Bell(float Waist, float Narrow, float Wide, float Swell)
    {
        Body()
            .MoveTo(At(50 - Wide, 77))
            .CubicTo(At(50 - Wide + Swell, 68), At(50 - Narrow - 2, Waist + 8), At(50 - Narrow, Waist))
            .LineTo(At(50 + Narrow, Waist))
            .CubicTo(At(50 + Narrow + 2, Waist + 8), At(50 + Wide - Swell, 68), At(50 + Wide, 77))
            .Close();
    }

    float             U;
    std::vector<Part> Parts;
};

void DrawPawn(Drawing &D)
{
    D.Foot();
    D.Bell(54, 8, 18, 3);
    D.Oval(50, 53, 14, 4.2f);
    D.Round(50, 39, 13);
}

void DrawRook(Drawing &D)
{
    D.Foot();
    D.Body()
        .MoveTo(D.At(31, 77))
        .QuadTo(D.At(34, 60), D.At(35, 44))
        .LineTo(D.At(65, 44))
        .QuadTo(D.At(66, 60), D.At(69, 77))
        .Close();
    D.Body().AddRoundedRect({29 * D.U, 38 * D.U, 42 * D.U, 7 * D.U}, painter::core::Corners::All(2 * D.U));
    Path &Top = D.Body();
    Top.MoveTo(D.At(28, 39)).LineTo(D.At(28, 18)).LineTo(D.At(37, 18)).LineTo(D.At(37, 25)).LineTo(D.At(45.5f, 25));
    Top.LineTo(D.At(45.5f, 18)).LineTo(D.At(54.5f, 18)).LineTo(D.At(54.5f, 25)).LineTo(D.At(63, 25)).LineTo(D.At(63, 18));
    Top.LineTo(D.At(72, 18)).LineTo(D.At(72, 39)).Close();
    D.Line().MoveTo(D.At(35, 62)).LineTo(D.At(65, 62));
}

void DrawKnight(Drawing &D)
{
    D.Foot();
    D.Body()
        .MoveTo(D.At(30, 77))
        .CubicTo(D.At(31, 66), D.At(37, 58), D.At(44, 52))
        .CubicTo(D.At(38, 52), D.At(28, 55), D.At(23, 52))
        .CubicTo(D.At(16, 50), D.At(15, 42), D.At(20, 37))
        .CubicTo(D.At(26, 29), D.At(34, 22), D.At(42, 18))
        .LineTo(D.At(46, 7))
        .LineTo(D.At(53, 16))
        .CubicTo(D.At(65, 20), D.At(73, 34), D.At(73, 48))
        .CubicTo(D.At(74, 60), D.At(72, 70), D.At(70, 77))
        .Close();
    /* The mane down the back of the neck, the eye, the nostril, the mouth. */
    D.Line().MoveTo(D.At(55, 22)).CubicTo(D.At(63, 28), D.At(67, 38), D.At(67, 52));
    D.Line().MoveTo(D.At(24, 49)).LineTo(D.At(31, 48));
    D.Oval(36, 30, 2.6f, 2.2f);
    D.Oval(22.5f, 43, 1.6f, 1.6f);
}

void DrawBishop(Drawing &D)
{
    D.Foot();
    D.Bell(57, 6, 16, 4);
    D.Oval(50, 56, 12.5f, 4);
    D.Body()
        .MoveTo(D.At(50, 15))
        .CubicTo(D.At(64, 26), D.At(66, 41), D.At(58, 52))
        .LineTo(D.At(42, 52))
        .CubicTo(D.At(34, 41), D.At(36, 26), D.At(50, 15))
        .Close();
    D.Round(50, 11.5f, 4.5f);
    D.Line().MoveTo(D.At(56, 25)).LineTo(D.At(47, 39));
}

void DrawQueen(Drawing &D)
{
    D.Foot();
    D.Bell(46, 8, 20, 6);
    D.Oval(50, 45, 14.5f, 4.2f);
    constexpr float Points[5][2] = {{25, 21}, {37.5f, 15}, {50, 12}, {62.5f, 15}, {75, 21}};
    constexpr float Dips[4][2]   = {{36, 32}, {44, 29}, {56, 29}, {64, 32}};
    Path &Crown = D.Body();
    Crown.MoveTo(D.At(36, 42)).LineTo(D.At(Points[0][0], Points[0][1]));
    for (int At = 0; At < 4; At++) Crown.LineTo(D.At(Dips[At][0], Dips[At][1])).LineTo(D.At(Points[At + 1][0], Points[At + 1][1]));
    Crown.LineTo(D.At(64, 42)).Close();
    for (const auto &One : Points) D.Round(One[0], One[1], 3.6f);
}

void DrawKing(Drawing &D)
{
    D.Foot();
    D.Bell(48, 8, 19, 6);
    D.Oval(50, 47, 14.5f, 4.2f);
    D.Body()
        .MoveTo(D.At(33, 44))
        .CubicTo(D.At(27, 31), D.At(36, 22), D.At(50, 22))
        .CubicTo(D.At(64, 22), D.At(73, 31), D.At(67, 44))
        .Close();
    D.Line().MoveTo(D.At(31, 35)).QuadTo(D.At(50, 31), D.At(69, 35));
    Path &Cross = D.Body();
    Cross.MoveTo(D.At(47, 4)).LineTo(D.At(53, 4)).LineTo(D.At(53, 9)).LineTo(D.At(59, 9)).LineTo(D.At(59, 15)).LineTo(D.At(53, 15));
    Cross.LineTo(D.At(53, 23)).LineTo(D.At(47, 23)).LineTo(D.At(47, 15)).LineTo(D.At(41, 15)).LineTo(D.At(41, 9)).LineTo(D.At(47, 9)).Close();
}

gui::Stroke Edge(float Width, Color With)
{
    gui::Stroke Made;
    Made.Width = Width;
    Made.Join  = ITEM_JOIN_ROUND;
    Made.Cap   = ITEM_CAP_ROUND;
    Made.With  = gui::Fill::Of(With);
    return Made;
}

gui::Fill Lit(float Size, Color Top, Color Bottom)
{
    gui::Fill Made;
    Made.What   = gui::Fill::Kind::Radial;
    Made.From   = {Size * 0.42f, Size * 0.3f};
    Made.Focus  = {Size * 0.36f, Size * 0.22f};
    Made.Radius = Size * 0.78f;
    Made.Stops  = {gui::Stop{0, Top}, gui::Stop{1, Bottom}};
    return Made;
}

}  // namespace

Figure::Figure(gui::Group &Into, Piece Is, float Points) : Group(Into.Subgroup()), Of(Is), Size(Points)
{
    Group.SetAnchor({Size / 2, Size / 2});
    Draw();
}

void Figure::Become(Piece Other)
{
    if (Other == Of) return;
    Of = Other;
    Parts.clear();
    Draw();
}

void Figure::Draw()
{
    Drawing D(Size);
    switch (KindOf(Of)) {
    case Pawn: DrawPawn(D); break;
    case Rook: DrawRook(D); break;
    case Knight: DrawKnight(D); break;
    case Bishop: DrawBishop(D); break;
    case Queen: DrawQueen(D); break;
    default: DrawKing(D); break;
    }
    const bool  Light = SideOf(Of) == White;
    const Color Line  = Light ? ink::IvoryEdge : ink::EbonyEdge;
    const float Width = 2.4f * D.U;
    Parts.reserve(D.Parts.size() + 1);
    /* The shadow it casts on the square. */
    gui::Fill Under;
    Under.What   = gui::Fill::Kind::Radial;
    Under.From   = {Size / 2, Size * 0.88f};
    Under.Radius = Size * 0.34f;
    Under.Stops  = {gui::Stop{0, Color::Rgba(0, 0, 0, 0x70)}, gui::Stop{1, Color::Rgba(0, 0, 0, 0)}};
    Parts.push_back(Group.Ellipse());
    Parts.back().SetPlace({Size * 0.14f, Size * 0.8f}).SetSize({Size * 0.72f, Size * 0.16f}).SetFill(Under);
    for (const Part &One : D.Parts) {
        Parts.push_back(Group.Outline(One.Shape));
        if (One.Line) Parts.back().SetFill(gui::Fill::Of(Color::Through())).SetStroke(Edge(Width * 0.8f, Light ? Line : ink::EbonyLine));
        else Parts.back().SetFill(Light ? Lit(Size, ink::Ivory, ink::IvoryDim) : Lit(Size, ink::Ebony, ink::EbonyDim)).SetStroke(Edge(Width, Line));
    }
}

}  // namespace chess
