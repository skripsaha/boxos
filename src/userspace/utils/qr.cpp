/* qr — a QR Code of what is said, on the screen where the words were.
 *
 *   qr hello there               the code, under the line, and the prompt after it
 *   qr https://example.org       a phone's camera reads it off the glass
 *   qr wifi -> code.txt          the code as text into code.txt: ## dark, blanks light
 *
 * The code is as small as the words allow and as robust as that size allows
 * (qr_code.cpp). On the glass a module is half a cell high and a cell wide —
 * square — dark on white with the quiet zone round it a phone needs; it is
 * painted into the console's own text, so it stays until it scrolls away
 * and the prompt comes back beneath it, as passport's portrait does. */

#include <algorithm>
#include <format>
#include <new>
#include <string>
#include <vector>

#include "box/form.h"
#include "box/print.h"

#include "painter/core/color.h"
#include "painter/tui/glyph.h"
#include "painter/tui/console_surface.h"
#include "qr_code.h"

BOX_FORM("takes text \"the words to make a code of\"\n");

namespace {

constexpr int  QuietMost  = 4;   /* what the standard asks round a code */
constexpr int  QuietLeast = 2;   /* what a phone still reads, when the glass is small */
constexpr int  Indent     = 2;
constexpr int  TextQuiet  = 4;
using Color = painter::core::Color;
namespace Colors = painter::core::Colors;

void Say(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_to_screen(Line.c_str());
}

const char *LevelName(QrLevel Level)
{
    switch (Level) {
    case QrLevel::Low:      return "L";
    case QrLevel::Medium:   return "M";
    case QrLevel::Quartile: return "Q";
    default:                return "H";
    }
}

std::string Caption(const QrCode &Code, std::size_t Bytes)
{
    return std::format("qr: {} byte{}, version {} ({} x {}), error correction {}", Bytes, Bytes == 1 ? "" : "s",
                       Code.Version, Code.Size, Code.Size, LevelName(Code.Level));
}

/* Whether the module at (X, Y) of the code with its quiet zone is dark. */
bool DarkAt(const QrCode &Code, int Quiet, int X, int Y)
{
    X -= Quiet;
    Y -= Quiet;
    return X >= 0 && Y >= 0 && X < Code.Size && Y < Code.Size && Code.Dark(X, Y);
}

/* The code as text for a file: two characters a module, so it is square in
 * a fixed font. */
std::string AsText(const QrCode &Code, std::size_t Bytes)
{
    std::string Out;
    int         Side = Code.Size + 2 * TextQuiet;
    for (int Y = 0; Y < Side; Y++) {
        for (int X = 0; X < Side; X++) Out += DarkAt(Code, TextQuiet, X, Y) ? "##" : "  ";
        while (!Out.empty() && Out.back() == ' ') Out.pop_back();
        Out += '\n';
    }
    Out += Caption(Code, Bytes) + '\n';
    return Out;
}

/* The rows of the console from the cursor down, scrolled up first when the
 * code would not fit under it; the row it begins at, or -1. */
std::expected<int, box::error> MakeRoom(int Rows, painter::tui::ConsoleSurface &Screen)
{
    std::expected<painter::core::Point, box::error> Cursor = Screen.Cursor();
    if (!Cursor) return std::unexpected(Cursor.error());
    int Top = Cursor->Y, Bottom = Screen.Size().Height - Rows - 1;   /* a row left for the prompt */
    for (; Top > Bottom; Top--)
        if (auto Scrolled = Screen.ScrollUp(); !Scrolled) return std::unexpected(Scrolled.error());
    return Top;
}

bool Paint(const QrCode &Code, int Quiet, painter::tui::ConsoleSurface &Screen)
{
    int Side = Code.Size + 2 * Quiet, Rows = (Side + 1) / 2;
    std::expected<int, box::error> Top = MakeRoom(Rows + 1, Screen);
    if (!Top) return false;

    std::expected<painter::tui::Frame, box::error> Created = painter::tui::Frame::Create({Side, Rows});
    if (!Created) return false;
    painter::tui::Frame &Frame = *Created;
    for (int Row = 0; Row < Rows; Row++) {
        for (int X = 0; X < Side; X++) {
            painter::tui::Cell Cell;
            Cell.Glyph       = painter::tui::Glyphs::UpperHalf;
            Cell.Foreground = DarkAt(Code, Quiet, X, Row * 2) ? Colors::Black : Colors::White;
            Cell.Background = DarkAt(Code, Quiet, X, Row * 2 + 1) ? Colors::Black : Colors::White;
            Frame.Put({X, Row}, Cell);
        }
    }
    if (!Screen.Paint({Indent, *Top}, Frame)) return false;
    return static_cast<bool>(Screen.SetCursor({0, *Top + Rows}));
}

/* The biggest version whose code fits the glass with the least quiet zone,
 * under the code a line for the caption and one for the prompt. */
int MostVersionFor(const painter::core::Size &Screen)
{
    int Side = std::min(Screen.Width - Indent, (Screen.Height - 2) * 2);
    return (Side - 2 * QuietLeast - 17) / 4;
}

int Run()
{
    const char *Text = form_text(0);
    if (!Text) return 1;
    std::string Words(Text);
    if (Words.empty()) {
        Say("qr: there are no words to make a code of");
        return 1;
    }

    if (form_screen_file() != 0) {
        std::optional<QrCode> Code = QrCode::Make(Words, QrLevel::Low, 40);
        if (!Code) {
            Say(std::format("qr: {} bytes are more than a QR Code holds ({} at most)", Words.size(),
                            QrCode::Capacity(40, QrLevel::Low)));
            return 1;
        }
        std::string Out = AsText(*Code, Words.size());
        print_bytes(Out.data(), Out.size());
        return 0;
    }

    std::expected<painter::tui::ConsoleSurface, painter::tui::StageError> Opened =
        painter::tui::ConsoleSurface::Open(30, 10);
    if (!Opened) {
        Say(Opened.error().Describe("qr"));
        return 1;
    }
    painter::tui::ConsoleSurface Screen = std::move(*Opened);
    const painter::core::Size Dimensions = Screen.Size();
    int Most = std::min(40, MostVersionFor(Dimensions));
    if (Most < 1) {
        Say(std::format("qr: a screen of {} by {} is too small for a code", Dimensions.Width, Dimensions.Height));
        return 1;
    }
    std::optional<QrCode> Code = QrCode::Make(Words, QrLevel::Low, Most);
    if (!Code) {
        Say(std::format("qr: {} bytes do not fit this screen; {} do, or `qr ... -> file` takes up to {}", Words.size(),
                        QrCode::Capacity(Most, QrLevel::Low), QrCode::Capacity(40, QrLevel::Low)));
        return 1;
    }
    int Room  = std::min(Dimensions.Width - Indent, (Dimensions.Height - 2) * 2) - Code->Size;
    int Quiet = std::clamp(Room / 2, QuietLeast, QuietMost);
    if (!Paint(*Code, Quiet, Screen)) {
        Say("qr: the screen would not take the code");
        return 1;
    }
    if (!Screen.Commit()) {
        Say("qr: the code could not be committed to the console");
        return 1;
    }
    Say(Caption(*Code, Words.size()));
    return 0;
}

}

int main()
{
    try {
        return Run();
    } catch (const std::bad_alloc &) {
        Say("qr: there is not enough memory for the code");
        return 1;
    }
}
