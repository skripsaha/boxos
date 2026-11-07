/* typer_screen.cpp — the Painter stage, frame, and keyboard owned by typer. */

#include <format>

#include "typer_screen.h"

using painter::tui::TextStyle;

std::optional<std::string> Screen::Take()
{
    std::expected<painter::tui::Stage, painter::tui::StageError> Opened =
        painter::tui::Stage::Open(ScreenColsLeast, ScreenRowsLeast);
    if (!Opened) return Opened.error().Describe("typer");
    Stage.emplace(std::move(*Opened));
    Rows = static_cast<std::uint32_t>(Stage->Size().Height);
    Cols = static_cast<std::uint32_t>(Stage->Size().Width);
    return std::nullopt;
}

void Screen::Give()
{
    if (!Stage) return;
    (void)Stage->Close();
    Stage.reset();
}

void Screen::Put(std::uint32_t Row, std::uint32_t Col, char Ch, PainterColor Fg, PainterColor Bg)
{
    if (Row >= Rows || Col >= Cols) return;
    Stage->FrameBuffer().Put({static_cast<std::int32_t>(Col), static_cast<std::int32_t>(Row)},
                              painter::tui::Cell{Ch, Fg, Bg});
}

void Screen::Fill(std::uint32_t Row, std::uint32_t Col, std::uint32_t Width, char Ch, PainterColor Fg,
                  PainterColor Bg)
{
    for (std::uint32_t I = 0; I < Width; I++) Put(Row, Col + I, Ch, Fg, Bg);
}

void Screen::Clear(PainterColor FillColor)
{
    Stage->FrameBuffer().Clear(TextStyle{FillColor, FillColor});
}

std::uint32_t Screen::Write(std::uint32_t Row, std::uint32_t Col, std::string_view Text, PainterColor Fg,
                            PainterColor Bg)
{
    for (char Ch : Text) {
        if (Col >= Cols) break;
        Put(Row, Col++, Ch, Fg, Bg);
    }
    return Col;
}

std::uint32_t Screen::Centre(std::uint32_t Row, std::string_view Text, PainterColor Fg, PainterColor Bg)
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

bool Screen::Flush()
{
    if (!Stage) return false;
    if (!Stage->SetCursor({static_cast<std::int32_t>(Cols - 1), static_cast<std::int32_t>(Rows - 1)})) return false;
    return static_cast<bool>(Stage->Present());
}

Heard Screen::Listen(painter::tui::KeyEvent &Key, std::uint32_t TimeoutMs)
{
    if (!Stage) return Heard::Gone;
    painter::tui::Event Event = Stage->Wait(TimeoutMs);
    if (Event.Kind == painter::tui::EventKind::Quiet) return Heard::Quiet;
    if (Event.Kind == painter::tui::EventKind::Gone) return Heard::Gone;
    Key = Event.Key;
    return Heard::Key;
}
