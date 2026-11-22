/* board.cpp — the board, its marks and its pieces as items. See board.h. */

#include "board.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

#include "look.h"

namespace chess {

namespace {

using namespace std::chrono_literals;
using painter::core::Color;
using painter::core::Corners;
using painter::core::PointF;

constexpr float FrameShare = 0.42f;   // the frame, of a square
constexpr Kind  Promotions[4] = {Queen, Rook, Bishop, Knight};

gui::Fill Diagonal(float Long, Color From, Color To)
{
    gui::Fill Made;
    Made.What  = gui::Fill::Kind::Linear;
    Made.From  = {0, 0};
    Made.To    = {Long, Long};
    Made.Stops = {gui::Stop{0, From}, gui::Stop{1, To}};
    return Made;
}

gui::Stroke Line(float Width, Color With)
{
    gui::Stroke Made;
    Made.Width = Width;
    Made.Join  = ITEM_JOIN_ROUND;
    Made.With  = gui::Fill::Of(With);
    return Made;
}

gui::TextStyle Lettering(float Size, Color With)
{
    gui::TextStyle Made;
    Made.Size   = Size;
    Made.Color  = With;
    Made.Weight = 600;
    return Made;
}

gui::TextBox Centred()
{
    gui::TextBox Made;
    Made.Align = ITEM_TEXT_CENTER;
    Made.Wrap  = false;
    return Made;
}

int CellOf(Square Of)
{
    return Index(Of);
}

Square SquareOf(int Cell)
{
    return SquareAt(Cell % 8, Cell / 8);
}

float Apart(int A, int B)
{
    return std::hypot(static_cast<float>(A % 8 - B % 8), static_cast<float>(A / 8 - B / 8));
}

}  // namespace

float BoardView::Across(float Square)
{
    return 8 * Square + 2 * Square * FrameShare;
}

BoardView::BoardView(gui::Area &On, float Square, PointF At, bool Flipped) : Screen(On), S(Square), F(Square * FrameShare), Turned(Flipped)
{
    Whole = Screen.Group();
    Whole.SetPlace(At);
    MakeFrame();
    MakeSquares();
    Marks   = Whole.Subgroup();
    Reaches = Whole.Subgroup();
    Pieces  = Whole.Subgroup();
    LastFrom = Marks.Rectangle();
    LastTo   = Marks.Rectangle();
    Picked   = Marks.Rectangle();
    for (gui::Shape *One : {&LastFrom, &LastTo}) One->SetSize({S, S}).SetFill(gui::Fill::Of(ink::LastMove)).Hide();
    Picked.SetSize({S, S}).SetFill(gui::Fill::Of(ink::Picked)).Hide();

    /* A king in check: a red glow under it, breathing. */
    gui::Fill Red;
    Red.What   = gui::Fill::Kind::Radial;
    Red.From   = {S * 0.6f, S * 0.6f};
    Red.Radius = S * 0.62f;
    Red.Stops  = {gui::Stop{0, ink::Check}, gui::Stop{0.55f, Color::Rgba(0xFF, 0x3B, 0x30, 0x90)}, gui::Stop{1, Color::Rgba(0xFF, 0x3B, 0x30, 0)}};
    Glow       = Marks.Ellipse();
    Glow.SetSize({S * 1.2f, S * 1.2f}).SetFill(Red).Hide();
    gui::Animation Breathe = gui::Animation::InOut(700ms);
    Breathe.Repeats        = gui::Animation::Forever;
    Breathe.Back           = true;
    Screen.Animate(Breathe, [&] { Glow.SetOpacity(0.45f); });

    Pointer = Whole.Rectangle();
    Pointer.SetSize({S, S})
        .SetCorners(Corners::All(S * 0.08f))
        .SetFill(gui::Fill::Of(Color::Through()))
        .SetStroke(Line(std::max(2.0f, S * 0.05f), ink::Cursor))
        .SetShadow(gui::Shadow{{0, 0}, S * 0.18f, 0, Color::Rgba(0x6F, 0xD0, 0xFF, 0xA0), false})
        .Hide();

    Offered = Whole.Subgroup();
    Card    = Offered.Rectangle();
    Card.SetCorners(Corners::All(S * 0.14f))
        .SetFill(gui::Fill::Of(Color::Rgba(0xFB, 0xF7, 0xEE, 0xF4)))
        .SetShadow(gui::Shadow{{0, S * 0.1f}, S * 0.4f, 0, Color::Rgba(0, 0, 0, 0xA0), false});
    Chosen = Offered.Rectangle();
    Chosen.SetSize({S * 0.92f, S * 0.92f}).SetCorners(Corners::All(S * 0.12f)).SetFill(gui::Fill::Of(Color::Rgba(0x6F, 0xD0, 0xFF, 0x70)));
    for (const Kind One : Promotions) Choices.push_back(std::make_unique<Figure>(Offered, PieceOf(White, One), S));
    Offered.Hide();
    Label();
}

void BoardView::MakeFrame()
{
    const float All = Across(S);
    gui::Shape &Wood = Still.emplace_back(Whole.Rectangle());
    Wood.SetSize({All, All})
        .SetCorners(Corners::All(F * 0.35f))
        .SetFill(Diagonal(All, ink::FrameLit, ink::FrameDim))
        .SetShadow(gui::Shadow{{0, S * 0.2f}, S * 0.7f, 0, Color::Rgba(0, 0, 0, 0x90), false});
    gui::Shape &Brass = Still.emplace_back(Whole.Rectangle());
    Brass.SetPlace({F * 0.62f, F * 0.62f})
        .SetSize({All - F * 1.24f, All - F * 1.24f})
        .SetCorners(Corners::All(F * 0.12f))
        .SetFill(gui::Fill::Of(Color::Through()))
        .SetStroke(Line(std::max(1.5f, S * 0.022f), ink::Brass));
}

void BoardView::MakeSquares()
{
    gui::Shape &Light = Still.emplace_back(Whole.Rectangle());
    Light.SetPlace({F, F}).SetSize({8 * S, 8 * S}).SetFill(Diagonal(8 * S, ink::Maple, ink::MapleDim));
    painter::core::Path Dark;
    for (int Row = 0; Row < 8; Row++)
        for (int Col = 0; Col < 8; Col++)
            if ((Row + Col) % 2 == 1) Dark.AddRect({Col * S, Row * S, S, S});
    gui::Shape &Walnut = Still.emplace_back(Whole.Outline(Dark));
    Walnut.SetPlace({F, F}).SetFill(Diagonal(8 * S, ink::Walnut, ink::WalnutDim));
    gui::Shape &Edge = Still.emplace_back(Whole.Rectangle());
    Edge.SetPlace({F, F}).SetSize({8 * S, 8 * S}).SetFill(gui::Fill::Of(Color::Through())).SetStroke(Line(1.5f, Color::Rgba(0, 0, 0, 0x80)));
    for (std::size_t At = 0; At < Names.size(); At++) {
        Names[At] = Whole.Text("", gui::Role::Label);
        Names[At].SetStyle(Lettering(std::max(10.0f, S * 0.2f), ink::Label)).SetBox(Centred());
    }
}

/* The files along the bottom and the ranks up the left, as the board is
 * turned. */
void BoardView::Label()
{
    const float Size = std::max(10.0f, S * 0.2f);
    for (int Col = 0; Col < 8; Col++) {
        const int File = Turned ? 7 - Col : Col;
        Names[static_cast<std::size_t>(Col)]
            .SetText(std::string(1, static_cast<char>('a' + File)))
            .SetPlace({F + Col * S, F + 8 * S + (F - Size * 1.25f) / 2})
            .SetSize({S, gui::ByContent});
        const int Rank = Turned ? Col : 7 - Col;
        Names[static_cast<std::size_t>(8 + Col)]
            .SetText(std::string(1, static_cast<char>('1' + Rank)))
            .SetPlace({0, F + Col * S + (S - Size * 1.25f) / 2})
            .SetSize({F, gui::ByContent});
    }
}

PointF BoardView::Corner(int Cell) const
{
    const int File = Cell % 8, Rank = Cell / 8;
    const int Col = Turned ? 7 - File : File, Row = Turned ? Rank : 7 - Rank;
    return {F + Col * S, F + Row * S};
}

void BoardView::Glide(Placed &One, int Cell, bool Smooth)
{
    gui::Group &Art   = One.Art->Whole();
    const PointF Goal = Corner(Cell);
    const bool   Was  = One.Cell >= 0;
    One.Cell          = Cell;
    if (!Smooth) {
        Art.SetPlace(Goal).SetOpacity(1).SetScale({1, 1});
        return;
    }
    if (!Was) {
        /* Back from the taken: it fades in where it stands. */
        Art.SetPlace(Goal).SetOpacity(0).SetScale({0.7f, 0.7f});
        Screen.Animate(gui::Animation::Out(280ms), [&] { Art.SetOpacity(1).SetScale({1, 1}); });
        return;
    }
    /* Over everything else on its way. */
    Screen.Restack(Art.Number(), Pieces.Number(), ITEM_TOP);
    Screen.Animate(gui::Animation::InOut(260ms), [&] { Art.SetPlace(Goal); });
}

void BoardView::Show(const Position &At, bool Smooth)
{
    std::array<Piece, 64> Want{};
    std::array<int, 64>   Holder;
    Holder.fill(-1);
    for (int Cell = 0; Cell < 64; Cell++) Want[static_cast<std::size_t>(Cell)] = At.At(SquareOf(Cell));
    std::vector<bool> Used(Figures.size(), false);
    /* What stands where it stood stays. */
    for (std::size_t One = 0; One < Figures.size(); One++) {
        const int Cell = Figures[One].Cell;
        if (Cell >= 0 && Want[static_cast<std::size_t>(Cell)] == Figures[One].Art->Is()) {
            Holder[static_cast<std::size_t>(Cell)] = static_cast<int>(One);
            Used[One]                              = true;
        }
    }
    /* Each square still wanting its piece takes, in this order, the nearest
     * of its kind that moved; one of its side that moved, which becomes it —
     * a promotion, or one taken back; one of its kind that was taken; one of
     * its side that was taken; or a new one. */
    const auto Nearest = [&](int Cell, auto &&Fits) {
        int Best = -1;
        for (std::size_t One = 0; One < Figures.size(); One++)
            if (!Used[One] && Fits(Figures[One]) &&
                (Best < 0 || Apart(Figures[One].Cell, Cell) < Apart(Figures[static_cast<std::size_t>(Best)].Cell, Cell)))
                Best = static_cast<int>(One);
        return Best;
    };
    for (int Pass = 0; Pass < 4; Pass++)
        for (int Cell = 0; Cell < 64; Cell++) {
            const Piece Is = Want[static_cast<std::size_t>(Cell)];
            if (Is == 0 || Holder[static_cast<std::size_t>(Cell)] >= 0) continue;
            const bool Standing = Pass < 2, Exact = Pass % 2 == 0;
            const int  Found    = Nearest(Cell, [&](const Placed &One) {
                return (One.Cell >= 0) == Standing && (Exact ? One.Art->Is() == Is : SideOf(One.Art->Is()) == SideOf(Is));
            });
            if (Found < 0) continue;
            Used[static_cast<std::size_t>(Found)] = true;
            Holder[static_cast<std::size_t>(Cell)] = Found;
            Figures[static_cast<std::size_t>(Found)].Art->Become(Is);
            Glide(Figures[static_cast<std::size_t>(Found)], Cell, Smooth);
        }
    for (int Cell = 0; Cell < 64; Cell++) {
        const Piece Is = Want[static_cast<std::size_t>(Cell)];
        if (Is == 0 || Holder[static_cast<std::size_t>(Cell)] >= 0) continue;
        Figures.push_back(Placed{std::make_unique<Figure>(Pieces, Is, S), -1});
        Used.push_back(true);
        Glide(Figures.back(), Cell, Smooth);
    }
    /* What is left standing was taken: it fades where it stood. */
    for (std::size_t One = 0; One < Figures.size(); One++) {
        Placed &Left = Figures[One];
        if (Used[One] || Left.Cell < 0) continue;
        Left.Cell        = -1;
        gui::Group &Art = Left.Art->Whole();
        if (Smooth) Screen.Animate(gui::Animation::Out(240ms), [&] { Art.SetOpacity(0).SetScale({0.6f, 0.6f}); });
        else Art.SetOpacity(0);
    }
}

void BoardView::Flip(bool On)
{
    if (On == Turned) return;
    Turned = On;
    Label();
    for (Placed &One : Figures)
        if (One.Cell >= 0) {
            gui::Group  &Art  = One.Art->Whole();
            const PointF Goal = Corner(One.Cell);
            Screen.Animate(gui::Animation::InOut(420ms), [&] { Art.SetPlace(Goal); });
        }
    Last(Shown);
    Mark(Picked, PickedAt);
    Check(KingAt);
    Pointer.SetPlace(Corner(CellOf(CursorAt)));
}

void BoardView::Mark(gui::Shape &One, std::optional<Square> On)
{
    if (!On) {
        One.Hide();
        return;
    }
    One.Show().SetPlace(Corner(CellOf(*On)));
}

void BoardView::Cursor(Square At, bool Visible)
{
    const bool Moved = At != CursorAt;
    CursorAt         = At;
    if (!Visible) {
        Pointer.Hide();
        return;
    }
    Pointer.Show();
    const PointF Goal = Corner(CellOf(At));
    if (Moved) Screen.Animate(gui::Animation::Spring(200ms, 0.08f), [&] { Pointer.SetPlace(Goal); });
    else Pointer.SetPlace(Goal);
}

void BoardView::Pick(std::optional<Square> From, const MoveList &Reach, const Position &At)
{
    PickedAt = From;
    Mark(Picked, From);
    std::size_t Used = 0;
    if (From)
        for (const Move One : Reach) {
            if (One.From != *From) continue;
            bool Again = false;
            for (const Move Before : Reach) {
                if (&Before == &One) break;
                Again |= Before.From == One.From && Before.To == One.To;
            }
            if (Again) continue;
            if (Used == Dots.size()) Dots.push_back(Reaches.Ellipse());
            gui::Shape  &Dot    = Dots[Used++];
            const PointF Corner_ = Corner(CellOf(One.To));
            const bool   Takes  = At.At(One.To) != 0 || One.Is(Move::InPassing);
            if (Takes)
                Dot.SetPlace({Corner_.X + S * 0.05f, Corner_.Y + S * 0.05f})
                    .SetSize({S * 0.9f, S * 0.9f})
                    .SetFill(gui::Fill::Of(Color::Through()))
                    .SetStroke(Line(S * 0.075f, ink::Reach));
            else
                Dot.SetPlace({Corner_.X + S * 0.35f, Corner_.Y + S * 0.35f})
                    .SetSize({S * 0.3f, S * 0.3f})
                    .SetFill(gui::Fill::Of(ink::Reach))
                    .SetStroke(Line(0, Color::Through()));
            Dot.Show().SetOpacity(0);
            Screen.Animate(gui::Animation::Out(160ms), [&] { Dot.SetOpacity(1); });
        }
    for (std::size_t Rest = Used; Rest < Dots.size(); Rest++) Dots[Rest].Hide();
}

void BoardView::Last(Move One)
{
    Shown = One;
    Mark(LastFrom, One ? std::optional<Square>(One.From) : std::nullopt);
    Mark(LastTo, One ? std::optional<Square>(One.To) : std::nullopt);
}

void BoardView::Check(std::optional<Square> King)
{
    KingAt = King;
    if (!King) {
        Glow.Hide();
        return;
    }
    const PointF At = Corner(CellOf(*King));
    Glow.Show().SetPlace({At.X - S * 0.1f, At.Y - S * 0.1f});
}

void BoardView::Offer(Square To, Side Of, int Picking)
{
    const PointF Top   = Corner(CellOf(To));
    const bool   Down  = Top.Y < F + 4 * S;   // at the top edge the choices go down, at the bottom up
    const float  First = Down ? Top.Y : Top.Y - 3 * S;
    Offered.Show();
    Card.SetPlace({Top.X - S * 0.06f, First - S * 0.06f}).SetSize({S * 1.12f, S * 4.12f});
    for (std::size_t At = 0; At < Choices.size(); At++) {
        const std::size_t Slot = Down ? At : Choices.size() - 1 - At;
        Choices[At]->Become(PieceOf(Of, Promotions[At]));
        Choices[At]->Whole().SetPlace({Top.X, First + static_cast<float>(Slot) * S});
    }
    const std::size_t Slot = Down ? static_cast<std::size_t>(Picking) : Choices.size() - 1 - static_cast<std::size_t>(Picking);
    const PointF      Goal{Top.X + S * 0.04f, First + static_cast<float>(Slot) * S + S * 0.04f};
    Screen.Animate(gui::Animation::Spring(180ms, 0.08f), [&] { Chosen.SetPlace(Goal); });
}

void BoardView::Close()
{
    Offered.Hide();
}

}  // namespace chess
