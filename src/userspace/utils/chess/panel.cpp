/* panel.cpp — the card beside the board, as items. See panel.h. Laid in
 * points from the card's corner. */

#include "panel.h"

#include <algorithm>
#include <chrono>
#include <format>

#include "look.h"

namespace chess {

namespace {

using namespace std::chrono_literals;
using painter::core::Color;
using painter::core::Corners;

constexpr float Pad = 24, SeatTall = 74, RowTall = 25, PrizeSize = 24, MovesTop = 222, StatusFromBottom = 104, KeysFromBottom = 64;

constexpr int  Starting[7] = {0, 8, 2, 2, 2, 1, 0};
constexpr int  Values[7]   = {0, 1, 3, 3, 5, 9, 0};
constexpr Kind ByWorth[5]  = {Queen, Rook, Bishop, Knight, Pawn};

const Color Plain = Color::Rgb(0xF2, 0xED, 0xE4);
const Color Warm  = Color::Rgb(0xFF, 0x9A, 0x6B);

gui::TextStyle Style(float Size, std::uint16_t Weight, std::optional<Color> With = std::nullopt)
{
    gui::TextStyle Made;
    Made.Size   = Size;
    Made.Weight = Weight;
    if (With) Made.Color = *With;
    return Made;
}

gui::TextBox Aligned(std::uint8_t Align, bool Wrap = false)
{
    gui::TextBox Made;
    Made.Align = Align;
    Made.Wrap  = Wrap;
    return Made;
}

}  // namespace

SidePanel::SidePanel(gui::Area &On, painter::core::PointF At, painter::core::PointF Room) : Screen(On), Size(Room)
{
    Whole = Screen.Group();
    Whole.SetPlace(At);
    Plate = Whole.Rectangle(gui::Role::Panel);
    Plate.SetSize(Size).SetCorners(Corners::All(18)).SetOpacity(0.94f);
    Plate.SetShadow(gui::Shadow{{0, 14}, 40, 0, Color::Rgba(0, 0, 0, 0x88), false});
    Marker = Whole.Rectangle(gui::Role::Selection);
    Marker.SetSize({Size.X - 20, SeatTall}).SetCorners(Corners::All(12)).Hide();

    Title = Whole.Text("Chess", gui::Role::Heading);
    Title.SetPlace({Pad, 20}).SetStyle(Style(30, 700));
    Subtitle = Whole.Text("", gui::Role::Hint);
    Subtitle.SetPlace({Pad, 62}).SetSize({Size.X - 2 * Pad, gui::ByContent}).SetStyle(Style(13, 500));

    for (int Of = 0; Of < 2; Of++) MakeSeat(Seats_[static_cast<std::size_t>(Of)], static_cast<Side>(Of));

    Heading = Whole.Text("Moves", gui::Role::Label);
    Heading.SetPlace({Pad, MovesTop - 28}).SetStyle(Style(13, 600));
    const int Count = std::max(1, static_cast<int>((SeatY(false) - 18 - MovesTop) / RowTall));
    Lines.resize(static_cast<std::size_t>(Count));
    for (int At_ = 0; At_ < Count; At_++) {
        Line     &One = Lines[static_cast<std::size_t>(At_)];
        const float Y = MovesTop + static_cast<float>(At_) * RowTall;
        One.Number    = Whole.Text("", gui::Role::Hint);
        One.Number.SetPlace({Pad, Y}).SetSize({40, gui::ByContent}).SetBox(Aligned(ITEM_TEXT_RIGHT)).SetStyle(Style(15, 500));
        One.First = Whole.Text("", gui::Role::Body);
        One.First.SetPlace({Pad + 56, Y}).SetStyle(Style(15, 500));
        One.Second = Whole.Text("", gui::Role::Body);
        One.Second.SetPlace({Pad + 56 + (Size.X - 2 * Pad - 56) / 2, Y}).SetStyle(Style(15, 500));
    }

    Said = Whole.Text("", gui::Role::Body);
    Said.SetPlace({Pad, Size.Y - StatusFromBottom}).SetSize({Size.X - 2 * Pad, gui::ByContent}).SetBox(Aligned(ITEM_TEXT_LEFT, true));
    Keys = Whole.Text("arrows move   enter picks   u takes back   f turns the board   n new game   esc leaves", gui::Role::Hint);
    Keys.SetPlace({Pad, Size.Y - KeysFromBottom}).SetSize({Size.X - 2 * Pad, gui::ByContent}).SetBox(Aligned(ITEM_TEXT_LEFT, true)).SetStyle(Style(12, 500));
    PlaceSeats();
}

void SidePanel::MakeSeat(Seat &One, Side Of)
{
    One.Disc = Whole.Ellipse();
    gui::Stroke Edge;
    Edge.Width = 1.5f;
    Edge.With  = gui::Fill::Of(Of == White ? ink::IvoryEdge : Color::Rgb(0x9A, 0x90, 0x86));
    One.Disc.SetSize({16, 16}).SetFill(gui::Fill::Of(Of == White ? ink::Ivory : ink::EbonyDim)).SetStroke(Edge);
    One.Name = Whole.Text("", gui::Role::Body);
    One.Name.SetStyle(Style(17, 600, Plain));
    One.State = Whole.Text("", gui::Role::Hint);
    One.State.SetSize({120, gui::ByContent}).SetBox(Aligned(ITEM_TEXT_RIGHT)).SetStyle(Style(13, 500));
    gui::Animation Breathe = gui::Animation::InOut(800ms);
    Breathe.Repeats        = gui::Animation::Forever;
    Breathe.Back           = true;
    Screen.Animate(Breathe, [&] { One.State.SetOpacity(0.35f); });
    One.Lead = Whole.Text("", gui::Role::Hint);
    One.Lead.SetSize({48, gui::ByContent}).SetBox(Aligned(ITEM_TEXT_RIGHT)).SetStyle(Style(14, 600));
    One.Row = Whole.Subgroup();
}

float SidePanel::SeatY(bool Top) const
{
    return Top ? 104 : Size.Y - 196;
}

void SidePanel::PlaceSeats()
{
    for (int Of = 0; Of < 2; Of++) {
        Seat       &One = Seats_[static_cast<std::size_t>(Of)];
        const bool  Top = (Of == Black) != Turned;
        const float Y   = SeatY(Top);
        One.Disc.SetPlace({Pad, Y + 13});
        One.Name.SetPlace({Pad + 26, Y + 8});
        One.State.SetPlace({Size.X - Pad - 120, Y + 10});
        One.Row.SetPlace({Pad + 26, Y + 38});
        One.Lead.SetPlace({Size.X - Pad - 48, Y + 42});
    }
    if (MarkerShown) Marker.SetPlace({10, SeatY((Marked == Black) != Turned)});
}

void SidePanel::Mode(std::string_view Text)
{
    Subtitle.SetText(Text);
}

void SidePanel::Seats(std::string_view White_, std::string_view Black_, bool Flipped)
{
    Seats_[White].Name.SetText(White_);
    Seats_[Black].Name.SetText(Black_);
    Turned = Flipped;
    PlaceSeats();
}

void SidePanel::Turn(Side ToMove, bool Thinking, bool Over)
{
    for (int Of = 0; Of < 2; Of++) Seats_[static_cast<std::size_t>(Of)].State.SetText(Of == ToMove && Thinking ? "thinking" : "");
    if (Over) {
        Marker.Hide();
        MarkerShown = false;
        return;
    }
    const float Goal  = SeatY((ToMove == Black) != Turned);
    const bool  Glide = MarkerShown && ToMove != Marked;
    Marked            = ToMove;
    Marker.Show();
    if (Glide) Screen.Animate(gui::Animation::Spring(320ms, 0.1f), [&] { Marker.SetPlace({10, Goal}); });
    else Marker.SetPlace({10, Goal});
    MarkerShown = true;
}

/* Each side's taken pieces are what the other side lacks of what it began
 * with — a pawn promoted counted as the pawn it was. */
void SidePanel::Taken(const Position &At)
{
    int Have[2][7] = {}, Material[2] = {0, 0};
    for (Square Of = 0; Of < 128; Of++) {
        if (!OnBoard(Of)) continue;
        const Piece One = At.At(Of);
        if (One == 0) continue;
        Have[SideOf(One)][KindOf(One)]++;
        Material[SideOf(One)] += Values[KindOf(One)];
    }
    for (int Taker = 0; Taker < 2; Taker++) {
        const int Lost = 1 - Taker;
        int       Promoted = 0, Missing[7] = {};
        for (const Kind One : {Knight, Bishop, Rook, Queen}) {
            Promoted += std::max(0, Have[Lost][One] - Starting[One]);
            Missing[One] = std::max(0, Starting[One] - Have[Lost][One]);
        }
        Missing[Pawn] = std::max(0, 8 - Have[Lost][Pawn] - Promoted);
        Seat       &One   = Seats_[static_cast<std::size_t>(Taker)];
        std::size_t Shown = 0;
        float       X     = 0;
        for (const Kind Is : ByWorth) {
            for (int Count = 0; Count < Missing[Is]; Count++) {
                if (Shown == One.Prizes.size()) One.Prizes.push_back(std::make_unique<Figure>(One.Row, PieceOf(static_cast<Side>(Lost), Is), PrizeSize));
                Figure &Prize = *One.Prizes[Shown++];
                Prize.Become(PieceOf(static_cast<Side>(Lost), Is));
                Prize.Whole().Show().SetPlace({X, 0});
                X += PrizeSize * 0.52f;
            }
            if (Missing[Is] > 0) X += PrizeSize * 0.3f;
        }
        for (std::size_t Rest = Shown; Rest < One.Prizes.size(); Rest++) One.Prizes[Rest]->Whole().Hide();
        const int Ahead = Material[Taker] - Material[Lost];
        One.Lead.SetText(Ahead > 0 ? std::format("+{}", Ahead) : std::string());
    }
}

void SidePanel::Moves(const std::vector<std::string> &Written)
{
    const int Count = static_cast<int>(Written.size());
    const int Full  = (Count + 1) / 2;
    const int From  = std::max(0, Full - static_cast<int>(Lines.size()));
    for (std::size_t Row = 0; Row < Lines.size(); Row++) {
        Line     &One  = Lines[Row];
        const int Move = From + static_cast<int>(Row);
        const int A = 2 * Move, B = 2 * Move + 1;
        One.Number.SetText(Move < Full ? std::format("{}.", Move + 1) : std::string());
        One.First.SetText(A < Count ? Written[static_cast<std::size_t>(A)] : std::string());
        One.Second.SetText(B < Count ? Written[static_cast<std::size_t>(B)] : std::string());
        /* The last move said brighter. */
        One.First.SetStyle(A == Count - 1 ? Style(15, 700, ink::Cursor) : Style(15, 500, Plain));
        One.Second.SetStyle(B == Count - 1 ? Style(15, 700, ink::Cursor) : Style(15, 500, Plain));
    }
}

void SidePanel::Status(std::string_view Text, bool Loud)
{
    Said.SetText(Text).SetStyle(Style(18, 650, Loud ? Warm : Plain));
}

}  // namespace chess
