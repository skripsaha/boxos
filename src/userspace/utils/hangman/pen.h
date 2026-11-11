/* pen.h — writing words on a board's paper: where a line of text starts, in
 * which colour, over which background, and how it lines up in the area it is
 * given. A piece of a line is a Run, in its own colour. */
#ifndef HANGMAN_PEN_H
#define HANGMAN_PEN_H

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

#include "painter/core/geometry.h"
#include "painter/tui/frame.h"
#include "painter/tui/text.h"

#include "numbers.h"

namespace hangman {

struct Run {
    std::string Text;
    Color       Ink;
};

struct Pen {
    painter::tui::Frame &Out;
    painter::core::Rect  Area;
    Color                Paper;

    static std::int32_t Cells(std::string_view Text) { return static_cast<std::int32_t>(painter::tui::CellsOf(Text)); }

    // The paper of Area laid, and a pen for it.
    static Pen Laid(painter::tui::Frame &Out, painter::core::Rect Area, Color Paper)
    {
        Out.Fill(Area, ' ', {Paper, Paper});
        return Pen{Out, Area, Paper};
    }

    std::int32_t Width(std::initializer_list<Run> Runs) const
    {
        std::int32_t Total = 0;
        for (const Run &Each : Runs) Total += Cells(Each.Text);
        return Total;
    }

    // The runs one after another from X, to the right edge of the area at the furthest.
    void From(std::int32_t X, std::int32_t Row, std::initializer_list<Run> Runs) const
    {
        for (const Run &Each : Runs)
            X += painter::tui::PutText(Out, painter::core::Point{X, Row}, Each.Text, std::max(0, Area.X + Area.Width - X), Each.Ink, Paper);
    }

    void Centred(std::int32_t Row, std::initializer_list<Run> Runs) const
    {
        From(Area.X + std::max(0, (Area.Width - Width(Runs)) / 2), Row, Runs);
    }

    // Against the right edge, a cell in from it.
    void Right(std::int32_t Row, std::initializer_list<Run> Runs) const
    {
        From(Area.X + std::max(0, Area.Width - Width(Runs) - 1), Row, Runs);
    }

    // Centred on the column X.
    void Over(std::int32_t X, std::int32_t Row, std::initializer_list<Run> Runs) const
    {
        From(X - Width(Runs) / 2, Row, Runs);
    }
};

}  // namespace hangman

#endif
