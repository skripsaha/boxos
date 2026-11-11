/* text.cpp — the words of one note (text.h). */

#include "text.h"

#include <algorithm>

#include "painter/tui/text.h"

namespace scratchpad {

namespace tui = painter::tui;

NoteText NoteText::FromBytes(std::string_view Bytes)
{
    NoteText Made;
    for (const char Byte : Bytes) {
        if (Byte == '\n') Made.Lines.emplace_back();
        else if (Byte == '\t') Made.Lines.back() += ' ';
        else if (Byte != '\r') Made.Lines.back() += Byte;
    }
    if (Made.Lines.size() > 1 && Made.Lines.back().empty()) Made.Lines.pop_back();
    Made.Caret = Place{Made.Lines.size() - 1, Made.Lines.back().size()};
    return Made;
}

std::string NoteText::Bytes() const
{
    std::size_t Last = Lines.size();
    while (Last > 0 && Lines[Last - 1].find_first_not_of(' ') == std::string::npos) Last--;
    std::string Out;
    for (std::size_t At = 0; At < Last; At++) {
        Out += Lines[At];
        Out += '\n';
    }
    return Out;
}

bool NoteText::Blank() const noexcept
{
    return std::all_of(Lines.begin(), Lines.end(),
                       [](const std::string &One) { return One.find_first_not_of(' ') == std::string::npos; });
}

std::size_t NoteText::Size() const noexcept
{
    std::size_t Total = Lines.size() - 1;
    for (const std::string &One : Lines) Total += One.size();
    return Total;
}

std::size_t NoteText::Before(const std::string &Line, std::size_t Byte) const noexcept
{
    if (Byte == 0) return 0;
    do Byte--;
    while (Byte > 0 && (static_cast<unsigned char>(Line[Byte]) & 0xC0) == 0x80);
    return Byte;
}

std::size_t NoteText::After(const std::string &Line, std::size_t Byte) const noexcept
{
    return std::min(Line.size(), Byte + tui::GlyphAt(Line, Byte).Length);
}

bool NoteText::Type(char Typed)
{
    if (Size() >= MostBytes) return false;
    Lines[Caret.Line].insert(Caret.Byte, 1, Typed);
    Caret.Byte++;
    Wanted = NoColumn;
    Edited = true;
    return true;
}

bool NoteText::NewLine()
{
    if (Size() >= MostBytes) return false;
    std::string &Here = Lines[Caret.Line];
    std::string  Rest = Here.substr(Caret.Byte);
    Here.erase(Caret.Byte);
    Lines.insert(Lines.begin() + static_cast<std::ptrdiff_t>(Caret.Line) + 1, std::move(Rest));
    Caret = Place{Caret.Line + 1, 0};
    Wanted = NoColumn;
    Edited = true;
    return true;
}

void NoteText::Backspace()
{
    Wanted = NoColumn;
    if (Caret.Byte > 0) {
        std::string &Here  = Lines[Caret.Line];
        const std::size_t Back = Before(Here, Caret.Byte);
        Here.erase(Back, Caret.Byte - Back);
        Caret.Byte = Back;
    } else if (Caret.Line > 0) {
        std::string &Above = Lines[Caret.Line - 1];
        Caret.Byte         = Above.size();
        Above += Lines[Caret.Line];
        Lines.erase(Lines.begin() + static_cast<std::ptrdiff_t>(Caret.Line));
        Caret.Line--;
    } else {
        return;
    }
    Edited = true;
}

void NoteText::Delete()
{
    Wanted = NoColumn;
    std::string &Here = Lines[Caret.Line];
    if (Caret.Byte < Here.size()) {
        Here.erase(Caret.Byte, After(Here, Caret.Byte) - Caret.Byte);
    } else if (Caret.Line + 1 < Lines.size()) {
        Here += Lines[Caret.Line + 1];
        Lines.erase(Lines.begin() + static_cast<std::ptrdiff_t>(Caret.Line) + 1);
    } else {
        return;
    }
    Edited = true;
}

void NoteText::Left()
{
    Wanted = NoColumn;
    if (Caret.Byte > 0) {
        Caret.Byte = Before(Lines[Caret.Line], Caret.Byte);
    } else if (Caret.Line > 0) {
        Caret.Line--;
        Caret.Byte = Lines[Caret.Line].size();
    }
}

void NoteText::Right()
{
    Wanted = NoColumn;
    if (Caret.Byte < Lines[Caret.Line].size()) {
        Caret.Byte = After(Lines[Caret.Line], Caret.Byte);
    } else if (Caret.Line + 1 < Lines.size()) {
        Caret = Place{Caret.Line + 1, 0};
    }
}

std::vector<Row> NoteText::Rows(std::int32_t Width) const
{
    const std::size_t Limit = Width > 1 ? static_cast<std::size_t>(Width - 1) : 1;
    std::vector<Row>  Out;
    for (std::size_t Number = 0; Number < Lines.size(); Number++) {
        const std::string &Line = Lines[Number];
        std::size_t        From = 0;
        for (;;) {
            std::size_t At = From, Cells = 0, Break = 0;
            bool        Fits = true;
            while (At < Line.size()) {
                const tui::Glyph Next = tui::GlyphAt(Line, At);
                if (Cells + Next.Width > Limit && At > From) {
                    Fits = false;
                    break;
                }
                Cells += Next.Width;
                At += Next.Length;
                if (Line[At - 1] == ' ') Break = At;
            }
            if (Fits) {
                Out.push_back(Row{Number, From, Line.size()});
                break;
            }
            const std::size_t To = Break > From ? Break : At;
            Out.push_back(Row{Number, From, To});
            From = To;
        }
    }
    return Out;
}

CaretSpot NoteText::Spot(std::int32_t Width) const
{
    const std::vector<Row> All = Rows(Width);
    for (std::size_t At = 0; At < All.size(); At++) {
        const Row &One = All[At];
        if (One.Line != Caret.Line || Caret.Byte < One.From) continue;
        if (Caret.Byte < One.To || One.To == Lines[One.Line].size())
            return CaretSpot{At, tui::CellsOf(std::string_view(Lines[One.Line]).substr(One.From, Caret.Byte - One.From))};
    }
    return {};
}

void NoteText::GoToRow(const std::vector<Row> &All, std::size_t Target)
{
    const Row         &One  = All[Target];
    const std::string &Line = Lines[One.Line];
    std::size_t        Byte = One.From + tui::BytesWithin(std::string_view(Line).substr(One.From, One.To - One.From), Wanted);
    if (Byte >= One.To && One.To != Line.size()) Byte = Before(Line, One.To);
    Caret = Place{One.Line, Byte};
}

void NoteText::Up(std::int32_t Width)
{
    const std::vector<Row> All  = Rows(Width);
    const CaretSpot        Here = Spot(Width);
    if (Wanted == NoColumn) Wanted = Here.Column;
    if (Here.Row == 0) {
        Caret = Place{};
        return;
    }
    GoToRow(All, Here.Row - 1);
}

void NoteText::Down(std::int32_t Width)
{
    const std::vector<Row> All  = Rows(Width);
    const CaretSpot        Here = Spot(Width);
    if (Wanted == NoColumn) Wanted = Here.Column;
    if (Here.Row + 1 >= All.size()) {
        Caret = Place{Lines.size() - 1, Lines.back().size()};
        return;
    }
    GoToRow(All, Here.Row + 1);
}

void NoteText::RowStart(std::int32_t Width)
{
    Wanted     = NoColumn;
    Caret.Byte = Rows(Width)[Spot(Width).Row].From;
}

void NoteText::RowEnd(std::int32_t Width)
{
    Wanted = NoColumn;
    const Row One = Rows(Width)[Spot(Width).Row];
    Caret.Byte =One.To == Lines[One.Line].size() ? One.To : Before(Lines[One.Line], One.To);
}

}  // namespace scratchpad
