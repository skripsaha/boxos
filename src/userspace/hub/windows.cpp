/* windows.cpp — a choice in a window, and the line of who lives behind. */

#include "windows.h"

#include <algorithm>
#include <format>

#include "box/print.h"

#include "painter/core/color.h"
#include "painter/tui/frame.h"
#include "painter/tui/keyboard.h"
#include "painter/tui/surface.h"
#include "painter/tui/text.h"
#include "painter/tui/window.h"

namespace hub::windows {

namespace {

namespace core = painter::core;
namespace tui  = painter::tui;

constexpr std::string_view ChooseHint = "Enter takes it  Esc none";

// The window of a choice, laid out once: where it lies, how many choices it
// shows at a time, and each choice as a row says it.
struct Layout {
    core::Rect               Area;
    std::size_t              Rows = 0;
    std::vector<std::string> Lines;
};

Layout LayOut(core::Size Screen, std::string_view Title, std::span<const std::string> Choices)
{
    Layout             Out;
    const std::size_t  Digits = std::to_string(Choices.size()).size();
    std::size_t        Widest = std::max(tui::CellsOf(Title), tui::CellsOf(ChooseHint)) + 2;
    for (std::size_t I = 0; I < Choices.size(); I++) {
        Out.Lines.push_back(std::format("{:>{}}  {}", I + 1, Digits, Choices[I]));
        Widest = std::max(Widest, tui::CellsOf(Out.Lines.back()));
    }
    Out.Rows = std::min<std::size_t>(Choices.size(), static_cast<std::size_t>(std::max(1, Screen.Height - 4)));
    const auto Width  = static_cast<std::int32_t>(std::min<std::size_t>(Widest + 4, std::max(8, Screen.Width - 2)));
    const auto Height = static_cast<std::int32_t>(Out.Rows) + 2;
    Out.Area          = tui::Centred(Screen, Width, Height);
    return Out;
}

void Paint(tui::Frame &Out, const Layout &Laid, std::string_view Title, std::size_t Top, std::size_t Marked)
{
    const std::string Note = Laid.Lines.size() > Laid.Rows ? std::format("{}/{}", Marked + 1, Laid.Lines.size())
                                                           : std::string();
    const core::Rect Inside = tui::DrawWindow(Out, {0, 0, Laid.Area.Width, Laid.Area.Height}, Title, Note, ChooseHint);
    for (std::size_t Row = 0; Row < Laid.Rows; Row++) {
        const std::size_t  Index = Top + Row;
        const core::Color  Under = Index == Marked ? tui::ChosenPaper : tui::WindowPaper;
        const std::int32_t Y     = Inside.Y + static_cast<std::int32_t>(Row);
        Out.Fill({Inside.X - 1, Y, Inside.Width + 2, 1}, tui::Cell{' ', tui::WindowInk, Under});
        tui::PutText(Out, {Inside.X, Y}, Laid.Lines[Index], Inside.Width, tui::WindowInk, Under);
    }
}

// Marked moved by a key that moves it; false for any other key.
bool Move(const tui::KeyEvent &Key, std::size_t Count, std::size_t Page, std::size_t &Marked)
{
    if (!Key.IsExtended()) return false;
    switch (Key.Scancode) {
    case tui::KeyCodes::Up:       Marked = Marked > 0 ? Marked - 1 : 0; return true;
    case tui::KeyCodes::Down:     Marked = std::min(Marked + 1, Count - 1); return true;
    case tui::KeyCodes::PageUp:   Marked -= std::min(Marked, Page); return true;
    case tui::KeyCodes::PageDown: Marked = std::min(Marked + Page, Count - 1); return true;
    case tui::KeyCodes::Home:     Marked = 0; return true;
    case tui::KeyCodes::End:      Marked = Count - 1; return true;
    default:                      return false;
    }
}

// The line of who lives behind, and what it says now.
std::optional<tui::Surface> BehindLayer;
std::string                 BehindSaid;
std::int32_t                BehindWidth = 0;   // the cells the line took last

// "behind: radio, [edit], copy — asks for the keyboard": the one on the
// screen in brackets.
std::string BehindLine(const std::vector<BehindOne> &Living, const std::optional<Which> &OnScreen)
{
    std::string Line = " behind: ";
    for (std::size_t I = 0; I < Living.size(); I++) {
        if (I > 0) Line += ", ";
        Line += Living[I].Program == OnScreen ? "[" + Living[I].Who + "]" : Living[I].Who;
        if (Living[I].Asks) Line += " — asks for the keyboard";
    }
    return Line + " ";
}

}  // namespace

std::optional<asking::Answer> Choose(std::string_view Title, std::span<const std::string> Choices,
                                     std::size_t &Chosen)
{
    if (Choices.empty()) return asking::Answer::Empty;
    std::expected<tui::Surface, tui::StageError> Over = tui::Surface::Open(1, 1, tui::SurfaceRole::Overlay);
    if (!Over) return std::nullopt;
    std::expected<tui::Keyboard, tui::StageError> Keys = tui::Keyboard::Open();
    if (!Keys) return std::nullopt;

    // A heading said before a list ends with a colon; a title on a frame does not.
    if (Title.ends_with(':')) Title.remove_suffix(1);
    const Layout            Laid   = LayOut(Over->Size(), Title, Choices);
    box::result<tui::Frame> Canvas = tui::Frame::Create({Laid.Area.Width, Laid.Area.Height});
    if (!Canvas) return std::nullopt;

    std::size_t Top = 0, Marked = 0;
    for (;;) {
        Paint(*Canvas, Laid, Title, Top, Marked);
        if (!Over->Composite({Laid.Area.X, Laid.Area.Y}, *Canvas)) return std::nullopt;

        const tui::Event Heard = Keys->Wait();
        if (Heard.Kind == tui::EventKind::Gone)
            return console_input_ended() ? asking::Answer::Ended : asking::Answer::NoOne;
        if (Heard.Kind != tui::EventKind::Key) continue;
        const tui::KeyEvent &Key = Heard.Key;

        if (Move(Key, Choices.size(), Laid.Rows, Marked)) {
            if (Marked < Top) Top = Marked;
            if (Marked >= Top + Laid.Rows) Top = Marked - Laid.Rows + 1;
            continue;
        }
        if (Key.IsExtended()) continue;
        if (Key.Ascii == tui::KeyCodes::Escape) return asking::Answer::Empty;
        if (Key.Ascii == tui::KeyCodes::Enter || Key.Ascii == '\r') {
            Chosen = Marked;
            return asking::Answer::Given;
        }
        if (Choices.size() <= 9 && Key.Ascii >= '1' && Key.Ascii < static_cast<char>('1' + Choices.size())) {
            Chosen = static_cast<std::size_t>(Key.Ascii - '1');
            return asking::Answer::Given;
        }
    }
}

void ShowBehind(const Look &Shown, const std::vector<BehindOne> &Living, const std::optional<Which> &OnScreen)
{
    const std::string Line =
        Shown.Behind == BehindShown::Line && !Living.empty() ? BehindLine(Living, OnScreen) : std::string();
    if (Line == BehindSaid) return;
    BehindSaid = Line;
    if (Line.empty()) {
        BehindLayer.reset();
        BehindWidth = 0;
        return;
    }
    if (!BehindLayer) {
        std::expected<tui::Surface, tui::StageError> Opened = tui::Surface::Open(1, 1, tui::SurfaceRole::Notice);
        if (!Opened) return;
        BehindLayer.emplace(std::move(*Opened));
    }
    // No wider than half the screen: the other half is the clock's corner.
    const auto Width = static_cast<std::int32_t>(
        std::min<std::size_t>(tui::CellsOf(Line), static_cast<std::size_t>(std::max(1, BehindLayer->Size().Width / 2))));
    // As wide as the line was, the cells it no longer takes given back to
    // what lies under them: one picture, never a blank one between two.
    box::result<tui::Frame> Canvas = tui::Frame::Create({std::max(Width, BehindWidth), 1});
    if (!Canvas) return;
    const core::Color Ink   = core::Color::FromRaw(Shown.BehindColor);
    const core::Color Paper = tui::WindowPaper;
    Canvas->ClearTransparent();
    Canvas->Fill({0, 0, Width, 1}, tui::Cell{' ', Ink, Paper});
    tui::PutText(*Canvas, {0, 0}, tui::Shortened(Line, static_cast<std::size_t>(Width)), Width, Ink, Paper);
    if (BehindLayer->Composite({0, 0}, *Canvas)) BehindWidth = Width;
}

}  // namespace hub::windows
