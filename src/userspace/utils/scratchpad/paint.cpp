/* paint.cpp — the pad's picture: a note on a dark desk, a strip at its top with
 * its name, the words on its paper, a strip at its foot with the keys or what
 * is being asked; a window of every key over them when asked. */

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "painter/tui/text.h"
#include "painter/tui/window.h"

#include "pad.h"

namespace scratchpad {

namespace {

namespace tui = painter::tui;
using painter::core::Point;
using painter::core::Rect;

constexpr Color DeskPaper   = Color::FromRaw(0x1A1A1A);
constexpr Color ShadowPaper = Color::FromRaw(0x0C0C0C);

// Of 255: how far the strips at the top and the foot go from the paper to the
// ink, and how far what is said quietly does.
constexpr std::uint8_t StripWeight = 48;
constexpr std::uint8_t QuietWeight = 180;

// Margins of the words from the note's edges: sides, in cells.
constexpr std::int32_t SideMargin = 2;

std::int32_t Cells(std::string_view Text)
{
    return static_cast<std::int32_t>(tui::CellsOf(Text));
}

void Say(tui::Frame &Out, Point At, std::string_view Text, std::int32_t Width, Color Ink, Color Under)
{
    tui::PutText(Out, At, Text, Width, Ink, Under);
}

}  // namespace

Rect Pad::Window() const
{
    const painter::core::Size Size = Screen.Size();
    return tui::Centred(Size, std::min(Shown.Width, Size.Width - 2), std::min(Shown.Height, Size.Height - 2));
}

Rect Pad::TextArea() const
{
    const Rect Area = Window();
    return Rect{Area.X + SideMargin, Area.Y + 1, std::max(1, Area.Width - 2 * SideMargin), std::max(1, Area.Height - 2)};
}

void Pad::Paint()
{
    tui::Frame  &Out    = Screen.FrameBuffer();
    Note        &One    = Here();
    const Sheet &Colors = One.Paints();
    const Rect   Area   = Window();
    const Color  Strip  = Color::Between(Colors.Paper, Colors.Ink, StripWeight);
    const Color  Quiet  = Color::Between(Colors.Paper, Colors.Ink, QuietWeight);

    Out.Clear({Colors.Paper, DeskPaper});
    Out.Fill(Rect{Area.X + 1, Area.Y + 1, Area.Width, Area.Height}, ' ', {Colors.Ink, ShadowPaper});
    Out.Fill(Area, ' ', {Colors.Ink, Colors.Paper});
    Out.Fill(Rect{Area.X, Area.Y, Area.Width, 1}, ' ', {Colors.Ink, Strip});
    Say(Out, Point{Area.X + 1, Area.Y}, "scratchpad", Area.Width - 2, Colors.Ink, Strip);

    const std::string Title = Notes.size() > 1 ? std::format("{}  {}/{}", One.Name(), At + 1, Notes.size()) : One.Name();
    const std::int32_t Room = std::max(0, Area.Width - 14);
    const std::string  Shortened = tui::Shortened(Title, static_cast<std::size_t>(Room));
    Say(Out, Point{Area.X + Area.Width - 1 - Cells(Shortened), Area.Y}, Shortened, Cells(Shortened), Colors.Ink, Strip);

    const Rect                  Words = TextArea();
    const std::vector<Row>      Rows  = One.Text.Rows(Words.Width);
    const CaretSpot             Spot  = One.Text.Spot(Words.Width);
    const std::size_t           High  = static_cast<std::size_t>(Words.Height);
    if (Spot.Row < One.Top) One.Top = Spot.Row;
    if (Spot.Row >= One.Top + High) One.Top = Spot.Row + 1 - High;
    for (std::size_t Index = 0; Index < High && One.Top + Index < Rows.size(); Index++) {
        const Row &Line = Rows[One.Top + Index];
        const std::string_view Bytes = One.Text.Line(Line.Line).substr(Line.From, Line.To - Line.From);
        Say(Out, Point{Words.X, Words.Y + static_cast<std::int32_t>(Index)}, Bytes, Words.Width, Colors.Ink, Colors.Paper);
    }
    if (One.Text.Blank() && Rows.size() == 1)
        Say(Out, Point{Words.X, Words.Y}, "write a note", Words.Width, Quiet, Colors.Paper);

    if (Now != Asking::Help)
        (void)Out.SetCaret(Point{Words.X + static_cast<std::int32_t>(Spot.Column),
                                 Words.Y + static_cast<std::int32_t>(Spot.Row - One.Top)});
    PaintFooter(Out, Rect{Area.X, Area.Y + Area.Height - 1, Area.Width, 1}, Colors);
    if (Now == Asking::Help) PaintHelp(Out);
}

void Pad::PaintFooter(tui::Frame &Out, Rect Area, const Sheet &Colors) const
{
    const Color Strip = Color::Between(Colors.Paper, Colors.Ink, StripWeight);
    const Color Quiet = Color::Between(Strip, Colors.Ink, QuietWeight);
    Out.Fill(Area, ' ', {Colors.Ink, Strip});
    const std::int32_t Room = Area.Width - 2;
    if (Now == Asking::Trash) {
        Say(Out, Point{Area.X + 1, Area.Y}, "move this note to the trash?  y  or any other key", Room, Colors.Ink, Strip);
        return;
    }
    if (!Status.empty()) {
        Say(Out, Point{Area.X + 1, Area.Y}, tui::Shortened(Status, static_cast<std::size_t>(Room)), Room, Colors.Ink, Strip);
        return;
    }
    std::vector<std::pair<std::string, std::string_view>> Keys = {
        {"esc", "save"}, {Spelled(KeyColor), "color"}, {Spelled(KeyTrash), "trash"}, {Spelled(KeyHelp), "keys"}};
    if (Notes.size() > 1) Keys.push_back({"pgup/pgdn", "notes"});
    std::int32_t X = Area.X + 1;
    for (const auto &[Key, Does] : Keys) {
        const std::int32_t Needs = Cells(Key) + 1 + Cells(Does) + 2;
        if (X + Needs > Area.X + Area.Width) break;
        X += static_cast<std::int32_t>(tui::PutText(Out, Point{X, Area.Y}, Key, Needs, Colors.Ink, Strip)) + 1;
        X += static_cast<std::int32_t>(tui::PutText(Out, Point{X, Area.Y}, Does, Needs, Quiet, Strip)) + 2;
    }
}

void Pad::PaintHelp(tui::Frame &Out) const
{
    const std::pair<std::string, std::string_view> Keys[] = {
        {"typing", "write on the note; it holds a few lines"},
        {"arrows", "move; up and down go by the rows of the note"},
        {"home, end", "the start and the end of the row"},
        {"enter", "a new line"},
        {"backspace, delete", "take a letter back, or the one under the caret"},
        {"page up, page down", "the note before, the note after"},
        {Spelled(KeyColor), "another random paper for this note"},
        {Spelled(KeyTrash), "move this note to the trash, once asked"},
        {Spelled(KeyAll), "page through every note there is"},
        {Spelled(KeyHelp), "these keys"},
        {"esc", "save and leave; an empty note is not kept"},
    };
    constexpr std::int32_t KeyWidth = 20;
    const painter::core::Size Size = Screen.Size();
    const Rect Area   = tui::Centred(Size, std::min(Size.Width - 2, 76), static_cast<std::int32_t>(std::size(Keys)) + 4);
    const Rect Inside = tui::DrawWindow(Out, Area, "the scratchpad's keys", "settings scratchpad keys", "any key closes this");
    std::int32_t Row = Inside.Y + 1;
    for (const auto &[Key, Does] : Keys) {
        if (Row >= Inside.Y + Inside.Height) break;
        Say(Out, Point{Inside.X + 2, Row}, Key, KeyWidth, tui::TitleInk, tui::WindowPaper);
        Say(Out, Point{Inside.X + 2 + KeyWidth, Row}, Does, Inside.Width - KeyWidth - 3, tui::WindowInk, tui::WindowPaper);
        Row++;
    }
}

}  // namespace scratchpad
