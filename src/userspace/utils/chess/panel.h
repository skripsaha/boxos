/* panel.h — the card beside the board: the game's name and how it is played,
 * the two players — the one whose move it is marked, the computer breathing
 * "thinking" while it does — what each has taken and who is ahead by how
 * much, the moves so far as they are written, what the position is (check,
 * mate, a draw and why), and the keys. Its seats are laid as the board is
 * turned: the player at the bottom of the board at the bottom of the card. */
#ifndef CHESS_PANEL_H
#define CHESS_PANEL_H

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "painter/gui/area.h"
#include "painter/gui/items.h"

#include "pieces.h"
#include "position.h"

namespace chess {

class SidePanel {
public:
    SidePanel(gui::Area &Screen, painter::core::PointF At, painter::core::PointF Size);

    void Mode(std::string_view Text);
    /* The players' names, and which sits at the top: black, unless Flipped. */
    void Seats(std::string_view White, std::string_view Black, bool Flipped);
    void Turn(Side ToMove, bool Thinking, bool Over);
    void Taken(const Position &At);
    void Moves(const std::vector<std::string> &Written);
    /* Loud: check, the end — said in a warmer colour. */
    void Status(std::string_view Text, bool Loud);

private:
    struct Seat {
        gui::Shape                            Disc;
        gui::Text                             Name, State, Lead;
        std::vector<std::unique_ptr<Figure>>  Prizes;
        gui::Group                            Row;
    };
    struct Line {
        gui::Text Number, First, Second;
    };

    void  MakeSeat(Seat &One, Side Of);
    void  PlaceSeats();
    float SeatY(bool Top) const;

    gui::Area              &Screen;
    painter::core::PointF   Size;
    gui::Group              Whole;
    gui::Shape              Plate, Marker;
    gui::Text               Title, Subtitle, Heading, Said, Keys;
    std::array<Seat, 2>     Seats_;
    std::vector<Line>       Lines;
    bool                    Turned = false;
    Side                    Marked = White;
    bool                    MarkerShown = false;
};

}  // namespace chess

#endif
