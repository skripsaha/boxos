/* typer_screen.cpp — the glass. A frame of cells, laid down by one vga_paint a
 * time: one op, and nothing of it in the log ring the serial line reads. */

#include <cstring>
#include <format>

#include "box/error.h"
#include "box/print.h"

#include "typer_glass.h"

std::optional<std::string> Screen::Take()
{
    vga_dimensions_t Glass;
    if (vga_getdimensions(&Glass) != 0) return std::string("This screen would not say how big it is.");
    if (Glass.rows == 0 || Glass.cols == 0) return std::string("This machine has no console to show text on.");
    if (Glass.cols < ScreenColsLeast || Glass.rows < ScreenRowsLeast)
        return std::format("This screen is {} columns by {} rows; typer needs at least {} by {}.", Glass.cols, Glass.rows,
                           ScreenColsLeast, ScreenRowsLeast);

    Rows = Glass.rows;
    Cols = Glass.cols;
    Cells.assign(static_cast<std::size_t>(Rows) * Cols, TextCell{});
    Ear = console_listen();
    if (Ear == TOUCH_TAG_INVALID) return std::string("This machine has no way to hear a key.");
    return std::nullopt;
}

/* Safe to call twice, and on a screen never taken. */
void Screen::Give()
{
    if (Ear == TOUCH_TAG_INVALID) return;
    console_unlisten();
    Ear = TOUCH_TAG_INVALID;
    (void)vga_clear_rgb(COLOR_LIGHT_GRAY, COLOR_BLACK);
    (void)vga_setcursor(0, 0);
}

void Screen::Put(std::uint32_t Row, std::uint32_t Col, char Ch, Color Fg, Color Bg)
{
    if (Row >= Rows || Col >= Cols) return;
    TextCell &Cell = Cells[Row * Cols + Col];
    Cell    = TextCell{};
    Cell.fg = Fg;
    Cell.bg = Bg;
    Cell.ch = Ch;
}

void Screen::Fill(std::uint32_t Row, std::uint32_t Col, std::uint32_t Width, char Ch, Color Fg, Color Bg)
{
    for (std::uint32_t I = 0; I < Width; I++) Put(Row, Col + I, Ch, Fg, Bg);
}

void Screen::Clear(Color Fill)
{
    for (std::uint32_t Row = 0; Row < Rows; Row++) this->Fill(Row, 0, Cols, ' ', Fill, Fill);
}

std::uint32_t Screen::Write(std::uint32_t Row, std::uint32_t Col, std::string_view Text, Color Fg, Color Bg)
{
    for (char Ch : Text) {
        if (Col >= Cols) break;
        Put(Row, Col++, Ch, Fg, Bg);
    }
    return Col;
}

std::uint32_t Screen::Centre(std::uint32_t Row, std::string_view Text, Color Fg, Color Bg)
{
    std::uint32_t Col = Text.size() < Cols ? static_cast<std::uint32_t>((Cols - Text.size()) / 2) : 0;
    Write(Row, Col, Text, Fg, Bg);
    return Col;
}

void Screen::Spread(std::uint32_t Row, const std::vector<Figure> &Figures)
{
    constexpr std::uint32_t Gap = 4;
    std::uint32_t Total = Figures.empty() ? 0 : Gap * static_cast<std::uint32_t>(Figures.size() - 1);
    for (const Figure &One : Figures) Total += static_cast<std::uint32_t>(One.Label.size() + 1 + One.Value.size());

    std::uint32_t Col = Total < Cols ? (Cols - Total) / 2 : 0;
    for (const Figure &One : Figures) {
        Col = Write(Row, Col, One.Label, HintInk, Paper) + 1;
        Col = Write(Row, Col, One.Value, One.Ink, Paper) + Gap;
    }
}

/* The caret goes to the corner: it is drawn wherever the kernel cursor stands
 * and cannot be hidden, so it is put where it means nothing. */
bool Screen::Flush()
{
    (void)vga_setcursor(static_cast<std::uint8_t>(Rows - 1), static_cast<std::uint8_t>(Cols - 1));
    return vga_paint(0, 0, static_cast<std::uint8_t>(Rows), static_cast<std::uint8_t>(Cols), Cells.data()) == 0;
}

/* -ERR_TIMEOUT is the only refusal that means time passed; any other means the
 * ear is gone. An event that is no key is time passed too, as far as the
 * caller is concerned: it asks again. */
Heard Screen::Listen(kb_event_t &Key, std::uint32_t TimeoutMs)
{
    Touch Event;
    int   Code = touch_await(Ear, &Event, TimeoutMs);
    if (Code == -ERR_TIMEOUT) return Heard::Quiet;
    if (Code != 0) return Heard::Gone;
    if (Event.payload_len < sizeof(kb_event_t)) return Heard::Quiet;
    std::memcpy(&Key, Event.payload, sizeof(Key));
    return Heard::Key;
}
