/* flame_menu.cpp — the menu over the fire, and the keys.
 *
 * ESC opens it and the fire keeps burning behind it. A serial console hands
 * keys over as bare bytes, so every action has a plain key beside the arrows
 * and Ctrl: tab, - and +, and a bare q to leave.
 */

#include <algorithm>
#include <format>
#include <string_view>

#include "box/color.h"

#include "flame.h"

namespace {

enum Row { RowWhere, RowColour, RowHeight, RowWind, RowSparks, RowWord, RowCount };

constexpr int MenuInner   = 40;
constexpr int MenuWidth   = MenuInner + 2;
constexpr int MarkAt      = 1;
constexpr int LabelAt     = 3;
constexpr int ValueAt     = 14;
constexpr int FieldWidth  = 24;
constexpr int ColourMax   = 7;

constexpr std::string_view Hints[] = {
    "  up/down, tab: choose    left/right, -/+: change",
    "  enter: type into a field    esc: close    q: leave",
    "  menu shut: 1-6 where, p colour, s sparks,",
    "  -/+ or up/down height, left/right or ,/. wind",
};
constexpr int HintCount = 4;
constexpr int Block     = 1 + RowCount + 1 + HintCount;

constexpr std::size_t Longest(const std::string_view *Lines, int Count)
{
    std::size_t Most = static_cast<std::size_t>(MenuWidth);
    for (int Line = 0; Line < Count; Line++)
        if (Lines[Line].size() > Most) Most = Lines[Line].size();
    return Most;
}

static_assert(Longest(Hints, HintCount) <= FlameMinCols, "a hint line is wider than the screen flame asks for");
static_assert(Block <= FlameMinRows, "the menu is taller than the screen flame asks for");

constexpr const char *Labels[RowCount] = {"where", "colour", "height", "wind", "sparks", "word"};

/* The console font is CP437 and a painted frame carries raw bytes, so the
 * box is drawn with its own single-line characters. */
constexpr char CornerTopLeft     = static_cast<char>(0xDA);
constexpr char CornerTopRight    = static_cast<char>(0xBF);
constexpr char CornerBottomLeft  = static_cast<char>(0xC0);
constexpr char CornerBottomRight = static_cast<char>(0xD9);
constexpr char Horizontal        = static_cast<char>(0xC4);
constexpr char Vertical          = static_cast<char>(0xB3);
constexpr char GaugeFull         = static_cast<char>(0xDB);
constexpr char GaugeEmpty        = static_cast<char>(0xB0);

/* A BIOS text console shows 16 colours and the kernel picks one by dominant
 * hue (BoxColorToVga4): any tint at all in a dark makes it a blue or a green.
 * So the box is neutral grey, which that rule reads as grey by lightness, and
 * each colour below says what it becomes there.
 *   Ink       #aaaaaa  light gray    Paper    #303030  dark gray
 *   ChosenInk #000000  black         ChosenBg #aaaaaa  light gray
 *   TitleInk  #ffb040  yellow        NoteInk  #e04040  light red
 *   FieldInk  #ffffff  white         FieldBg  #a05a00  brown       */
using PainterColor = painter::core::Color;

constexpr PainterColor Ink       = painter::core::Colors::LightGray;
constexpr PainterColor Paper     = PainterColor::Rgb(0x30, 0x30, 0x30);
constexpr PainterColor ChosenInk = painter::core::Colors::Black;
constexpr PainterColor ChosenBg  = painter::core::Colors::LightGray;
constexpr PainterColor TitleInk  = painter::core::Colors::Amber;
constexpr PainterColor FieldInk  = painter::core::Colors::White;
constexpr PainterColor FieldBg   = PainterColor::Rgb(0xA0, 0x5A, 0x00);
constexpr PainterColor NoteInk   = painter::core::Colors::Red;

struct Canvas {
    painter::tui::Frame &Cells;
    int       Rows;
    int       Cols;

    void Put(int Row, int Col, char Ch, PainterColor Fg, PainterColor Bg) const
    {
        if (Row < 0 || Col < 0 || Row >= Rows || Col >= Cols) return;
        Cells.Put({Col, Row}, painter::tui::Cell{Ch, Fg, Bg});
    }

    void Write(int Row, int Col, std::string_view Text, PainterColor Fg, PainterColor Bg) const
    {
        for (std::size_t At = 0; At < Text.size(); At++) Put(Row, Col + static_cast<int>(At), Text[At], Fg, Bg);
    }
};

int OriginRow(int Rows)
{
    return Rows > Block ? (Rows - Block) / 2 : 0;
}

int OriginCol(int Cols)
{
    int Width = static_cast<int>(Longest(Hints, HintCount));
    return Cols > Width ? (Cols - Width) / 2 : 0;
}

bool IsHex(char Ch)
{
    return (Ch >= '0' && Ch <= '9') || (Ch >= 'a' && Ch <= 'f') || (Ch >= 'A' && Ch <= 'F');
}

std::string ValueOf(int Row, const FlameSettings &Settings)
{
    switch (Row) {
    case RowWhere: return FlamePlaceName(Settings.Place);
    case RowColour: {
        if (Settings.Ink != FlameInk::Custom) return FlameInkName(Settings.Ink);
        char Spelled[8];
        color_format(Settings.Custom, Spelled);
        return Spelled;
    }
    case RowHeight:
        return std::string(static_cast<std::size_t>(Settings.Height), GaugeFull) +
               std::string(static_cast<std::size_t>(FlameHeightMax - Settings.Height), GaugeEmpty);
    case RowWind:
        if (Settings.Wind == 0) return "calm";
        return std::string(Settings.Wind < 0 ? "left " : "right ") +
               std::string(static_cast<std::size_t>(Settings.Wind < 0 ? -Settings.Wind : Settings.Wind),
                           Settings.Wind < 0 ? '<' : '>');
    case RowSparks: return Settings.Sparks ? "on" : "off";
    default: return Settings.Word.empty() ? "(none)" : Settings.Word;
    }
}

std::string_view Tail(const std::string &Text, std::size_t Width)
{
    std::string_view Whole = Text;
    return Whole.size() > Width ? Whole.substr(Whole.size() - Width) : Whole;
}

}

FlameOutcome FlameMenu::SetPlace(FlamePlace Place, FlameSettings &Settings)
{
    Settings.Place = Place;
    if (Place == FlamePlace::Word && Settings.Word.empty()) Settings.Word = "FLAME";
    FlameOutcome Outcome;
    Outcome.Rebuild = true;
    return Outcome;
}

FlameOutcome FlameMenu::Apply(int Row, int Delta, FlameSettings &Settings)
{
    FlameOutcome Outcome;
    switch (Row) {
    case RowWhere: {
        int Next = (static_cast<int>(Settings.Place) + Delta + FlamePlaceCount) % FlamePlaceCount;
        return SetPlace(static_cast<FlamePlace>(Next), Settings);
    }
    case RowColour: {
        int Next = static_cast<int>(Settings.Ink);
        do {
            Next = (Next + Delta + FlameInkCount) % FlameInkCount;
        } while (Next == static_cast<int>(FlameInk::Custom) && !Settings.HasCustom);
        Settings.Ink      = static_cast<FlameInk>(Next);
        Outcome.Repalette = true;
        break;
    }
    case RowHeight:
        Settings.Height = std::min(std::max(Settings.Height + Delta, FlameHeightMin), FlameHeightMax);
        break;
    case RowWind:
        Settings.Wind = std::min(std::max(Settings.Wind + Delta, -FlameWindMax), FlameWindMax);
        break;
    case RowSparks:
        Settings.Sparks = !Settings.Sparks;
        break;
    default:
        Note = "press enter";
        break;
    }
    return Outcome;
}

void FlameMenu::BeginEdit(const FlameSettings &Settings)
{
    if (Item != RowColour && Item != RowWord) {
        Note = "use -/+";
        return;
    }
    Editing = true;
    Note    = nullptr;
    Text    = Item == RowWord ? Settings.Word : std::string();
}

FlameOutcome FlameMenu::Accept(FlameSettings &Settings)
{
    FlameOutcome Outcome;
    if (Item == RowColour) {
        std::uint32_t Parsed;
        if (!color_parse(Text.c_str(), &Parsed)) {
            Note = "not a colour";
            return Outcome;
        }
        Settings.Custom    = Parsed;
        Settings.HasCustom = true;
        Settings.Ink       = FlameInk::Custom;
        Outcome.Repalette  = true;
    } else {
        Settings.Word   = Text;
        Outcome.Rebuild = true;
        if (Text.empty() && Settings.Place == FlamePlace::Word) Note = "nothing burns";
    }
    Editing = false;
    return Outcome;
}

FlameOutcome FlameMenu::Type(const painter::tui::KeyEvent &Key, FlameSettings &Settings)
{
    if (Key.IsExtended()) return {};

    char Ch = Key.Ascii;
    if (Ch == painter::tui::KeyCodes::Escape) {
        Editing = false;
        Note    = nullptr;
        return {};
    }
    if (Ch == painter::tui::KeyCodes::Enter) return Accept(Settings);

    Note = nullptr;
    if (Ch == painter::tui::KeyCodes::Backspace || Ch == 0x7F) {
        if (!Text.empty()) Text.pop_back();
        return {};
    }
    bool Fits = Item == RowColour ? (Ch == '#' || IsHex(Ch)) && Text.size() < ColourMax
                                  : Ch >= 0x20 && Ch < 0x7F && Text.size() < FlameWordMax;
    if (Fits) Text += Ch;
    return {};
}

FlameOutcome FlameMenu::Choose(const painter::tui::KeyEvent &Key, FlameSettings &Settings)
{
    Note = nullptr;
    if (Key.IsExtended()) {
        switch (Key.Scancode) {
        case painter::tui::KeyCodes::Up:    Item = (Item + RowCount - 1) % RowCount; break;
        case painter::tui::KeyCodes::Down:  Item = (Item + 1) % RowCount; break;
        case painter::tui::KeyCodes::Left:  return Apply(Item, -1, Settings);
        case painter::tui::KeyCodes::Right: return Apply(Item, 1, Settings);
        default: break;
        }
        return {};
    }
    switch (Key.Ascii) {
    case painter::tui::KeyCodes::Escape: Open = false; break;
    case painter::tui::KeyCodes::Tab:    Item = (Item + 1) % RowCount; break;
    case painter::tui::KeyCodes::Enter:  BeginEdit(Settings); break;
    case '-':        return Apply(Item, -1, Settings);
    case '+':
    case '=':        return Apply(Item, 1, Settings);
    default: break;
    }
    return {};
}

FlameOutcome FlameMenu::Quick(const painter::tui::KeyEvent &Key, FlameSettings &Settings)
{
    if (Key.IsExtended()) {
        switch (Key.Scancode) {
        case painter::tui::KeyCodes::Left:  return Apply(RowWind, -1, Settings);
        case painter::tui::KeyCodes::Right: return Apply(RowWind, 1, Settings);
        case painter::tui::KeyCodes::Up:    return Apply(RowHeight, 1, Settings);
        case painter::tui::KeyCodes::Down:  return Apply(RowHeight, -1, Settings);
        default: return {};
        }
    }
    char Ch = Key.Ascii;
    if (Ch >= '1' && Ch < '1' + FlamePlaceCount) return SetPlace(static_cast<FlamePlace>(Ch - '1'), Settings);
    switch (Ch) {
    case painter::tui::KeyCodes::Escape: Open = true; break;
    case 'p':
    case 'P':        return Apply(RowColour, 1, Settings);
    case 's':
    case 'S':        return Apply(RowSparks, 1, Settings);
    case '-':        return Apply(RowHeight, -1, Settings);
    case '+':
    case '=':        return Apply(RowHeight, 1, Settings);
    case ',':
    case '<':        return Apply(RowWind, -1, Settings);
    case '.':
    case '>':        return Apply(RowWind, 1, Settings);
    default: break;
    }
    return {};
}

FlameOutcome FlameMenu::OnKey(const painter::tui::KeyEvent &Key, FlameSettings &Settings)
{
    bool IsQ = !Key.IsExtended() && (Key.Ascii == 'q' || Key.Ascii == 'Q');
    if ((Key.IsCtrl() && IsQ) || (!Editing && IsQ)) {
        FlameOutcome Leaving;
        Leaving.Leave = true;
        return Leaving;
    }
    if (Editing) return Type(Key, Settings);
    return Open ? Choose(Key, Settings) : Quick(Key, Settings);
}

void FlameMenu::Draw(painter::tui::Frame &Frame, int Rows, int Cols, const FlameSettings &Settings) const
{
    Canvas Drawing{Frame, Rows, Cols};
    int    Top  = OriginRow(Rows);
    int    Left = OriginCol(Cols);

    auto Rule = [&](int Line, char First, char Last) {
        Drawing.Put(Line, Left, First, Ink, Paper);
        for (int At = 1; At <= MenuInner; At++) Drawing.Put(Line, Left + At, Horizontal, Ink, Paper);
        Drawing.Put(Line, Left + MenuInner + 1, Last, Ink, Paper);
    };

    Rule(Top, CornerTopLeft, CornerTopRight);
    Drawing.Write(Top, Left + 2, " flame ", TitleInk, Paper);
    if (Note) {
        std::string_view Said = Note;
        Drawing.Write(Top, Left + MenuInner - static_cast<int>(Said.size()) - 1, std::format(" {} ", Said), NoteInk, Paper);
    }

    for (int Line = 0; Line < RowCount; Line++) {
        bool  Chosen = Line == Item;
        PainterColor Fg = Chosen ? ChosenInk : Ink;
        PainterColor Bg = Chosen ? ChosenBg : Paper;
        int   Y      = Top + 1 + Line;

        Drawing.Put(Y, Left, Vertical, Ink, Paper);
        for (int At = 1; At <= MenuInner; At++) Drawing.Put(Y, Left + At, ' ', Fg, Bg);
        Drawing.Put(Y, Left + MenuInner + 1, Vertical, Ink, Paper);
        Drawing.Put(Y, Left + MarkAt + 1, Chosen ? '>' : ' ', Fg, Bg);
        Drawing.Write(Y, Left + LabelAt + 1, Labels[Line], Fg, Bg);

        int X = Left + ValueAt + 1;
        if (Editing && Chosen) {
            for (int At = 0; At < FieldWidth; At++) Drawing.Put(Y, X + At, ' ', FieldInk, FieldBg);
            Drawing.Write(Y, X, Tail(Text, FieldWidth - 1), FieldInk, FieldBg);
        } else {
            Drawing.Write(Y, X, Tail(ValueOf(Line, Settings), FieldWidth), Fg, Bg);
        }
    }

    Rule(Top + 1 + RowCount, CornerBottomLeft, CornerBottomRight);
    for (int Line = 0; Line < HintCount; Line++)
        Drawing.Write(Top + 2 + RowCount + Line, Left, Hints[static_cast<std::size_t>(Line)], Ink, Paper);
}

std::optional<painter::core::Point> FlameMenu::Caret(int Rows, int Cols) const
{
    if (!Open) return std::nullopt;
    int Row = OriginRow(Rows) + 1 + Item;
    int Col = OriginCol(Cols) + 1;
    if (Editing) {
        std::size_t Shown = std::min<std::size_t>(Text.size(), FieldWidth - 1);
        Col += ValueAt + static_cast<int>(Shown);
    } else {
        Col += MarkAt;
    }
    return painter::core::Point{std::min(Col, Cols - 1), std::min(Row, Rows - 1)};
}
