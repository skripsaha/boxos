/* stage.cpp — the glass a program owns (stage.h). */

#include <cstring>
#include <format>

#include "box/error.h"
#include "box/print.h"

#include "stage.h"

void StageSay(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_to_screen(Line.c_str());
}

std::optional<std::string> Stage::Take(std::string_view Who, int LeastCols, int LeastRows)
{
    vga_dimensions_t Glass;
    if (vga_getdimensions(&Glass) != 0) return std::string("This machine did not say how big its screen is.");
    if (Glass.rows == 0 || Glass.cols == 0) return std::string("This machine has no console to paint on.");
    if (Glass.cols < LeastCols || Glass.rows < LeastRows)
        return std::format("This screen is {} columns by {} rows; {} needs at least {} by {}.", Glass.cols,
                           Glass.rows, Who, LeastCols, LeastRows);

    Rows = Glass.rows;
    Cols = Glass.cols;
    Cells.assign(static_cast<std::size_t>(Rows) * Cols, TextCell{});
    Ear = console_listen();
    if (Ear == TOUCH_TAG_INVALID) return std::string("This machine has no way to hear a key.");
    return std::nullopt;
}

void Stage::Give()
{
    if (Ear == TOUCH_TAG_INVALID) return;
    console_unlisten();
    Ear = TOUCH_TAG_INVALID;
    (void)vga_clear_rgb(COLOR_LIGHT_GRAY, COLOR_BLACK);
    (void)vga_setcursor(0, 0);
}

void Stage::Clear(Color Paper)
{
    for (int Row = 0; Row < Rows; Row++) Fill(Row, 0, Cols, ' ', Paper, Paper);
}

void Stage::Put(int Row, int Col, char Ch, Color Ink, Color Paper)
{
    if (Row < 0 || Col < 0 || Row >= Rows || Col >= Cols) return;
    At(Row, Col) = TextCell{Ink, Paper, Ch, {0, 0, 0}};
}

void Stage::Fill(int Row, int Col, int Width, char Ch, Color Ink, Color Paper)
{
    for (int At = 0; At < Width; At++) Put(Row, Col + At, Ch, Ink, Paper);
}

int Stage::Write(int Row, int Col, std::string_view Text, Color Ink, Color Paper)
{
    for (char Ch : Text) Put(Row, Col++, Ch, Ink, Paper);
    return Col;
}

int Stage::Centre(int Row, std::string_view Text, Color Ink, Color Paper)
{
    int Col = static_cast<int>(Text.size()) < Cols ? (Cols - static_cast<int>(Text.size())) / 2 : 0;
    Write(Row, Col, Text, Ink, Paper);
    return Col;
}

void Stage::Pixels(const std::vector<Color> &Pixels)
{
    for (int Row = 0; Row < Rows; Row++) {
        for (int Col = 0; Col < Cols; Col++) {
            TextCell &Cell = At(Row, Col);
            Cell.ch = Glyph::UpperHalf;
            Cell.fg = Pixels[static_cast<std::size_t>(Row * 2) * Cols + Col];
            Cell.bg = Pixels[static_cast<std::size_t>(Row * 2 + 1) * Cols + Col];
        }
    }
}

/* The caret goes to the corner: it is drawn wherever the kernel cursor stands
 * and cannot be hidden, so it is put where it means nothing. */
bool Stage::Show()
{
    (void)vga_setcursor(static_cast<std::uint8_t>(Rows - 1), static_cast<std::uint8_t>(Cols - 1));
    return vga_paint(0, 0, static_cast<std::uint8_t>(Rows), static_cast<std::uint8_t>(Cols), Cells.data()) == 0;
}

/* -ERR_TIMEOUT is the only refusal that means time passed; any other means the
 * ear is gone. An event that is no key is time passed as well. */
StageHeard Stage::Listen(kb_event_t &Key, std::uint32_t TimeoutMs)
{
    Touch Event;
    int   Code = touch_await(Ear, &Event, TimeoutMs);
    if (Code == -ERR_TIMEOUT) return StageHeard::Quiet;
    if (Code != 0) return StageHeard::Gone;
    if (Event.payload_len < sizeof(kb_event_t)) return StageHeard::Quiet;
    std::memcpy(&Key, Event.payload, sizeof(Key));
    return StageHeard::Key;
}
