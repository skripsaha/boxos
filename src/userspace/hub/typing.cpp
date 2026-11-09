/* typing.cpp — a line cut word by word, as form_line.c cuts it. */

#include "typing.h"

#include "text.h"

namespace hub {

namespace {

// Where the word that begins at Start ends — past its closing quote, or at the
// blank or end that stops a bare one — and whether it ended at all: a quoted
// word whose quote is not closed runs on as it is typed.
bool WordEnd(std::string_view Line, std::size_t Start, std::size_t &End)
{
    if (Line[Start] == '"') {
        const std::size_t Close = Line.find('"', Start + 1);
        End = Close == std::string_view::npos ? Line.size() : Close + 1;
        return Close != std::string_view::npos;
    }
    End = Start;
    while (End < Line.size() && !text::Blank(Line[End])) End++;
    return true;
}

}  // namespace

Typing Typing::Of(std::string_view Line)
{
    Typing Typed;
    Typed.Line = Line;
    for (std::size_t At = 0;;) {
        while (At < Line.size() && text::Blank(Line[At])) At++;
        Typed.WordStart = Typed.PieceStart = At;
        if (At == Line.size()) return Typed;

        std::size_t End    = 0;
        const bool  Closed = WordEnd(Line, At, End);
        if (Line[At] == '"') {
            if (!Closed) {
                Typed.PieceStart = At + 1;
                Typed.Quoted     = true;
                return Typed;
            }
        } else {
            // A bare word is being typed until a blank ends it.
            if (End == Line.size()) {
                const std::size_t Comma = Line.substr(At).rfind(',');
                if (Comma != std::string_view::npos) Typed.PieceStart = At + Comma + 1;
                return Typed;
            }
        }

        const std::string_view Whole = Line.substr(At, End - At);
        if (Typed.Words == 0) {
            Typed.NameStart = At;
            Typed.NameEnd   = End;
        }
        Typed.AfterArrow = Whole == "->";
        if (Whole == "everywhere") Typed.Everywhere = true;
        Typed.Words++;
        At = End;
    }
}

std::string_view Typing::Name() const noexcept
{
    if (Words == 0) return Line.substr(WordStart);
    return Line.substr(NameStart, NameEnd - NameStart);
}

std::vector<std::string_view> Pieces(std::string_view Line)
{
    std::vector<std::string_view> Found;
    for (std::size_t At = 0;;) {
        while (At < Line.size() && text::Blank(Line[At])) At++;
        if (At == Line.size()) return Found;

        std::size_t End = 0;
        const bool  Closed = WordEnd(Line, At, End);
        if (Line[At] == '"') {
            const std::string_view Inside = Line.substr(At + 1, End - At - (Closed ? 2 : 1));
            if (!Inside.empty()) Found.push_back(Inside);
        } else {
            std::string_view Word = Line.substr(At, End - At);
            while (!Word.empty()) {
                const std::size_t Comma = Word.find(',');
                if (Comma != 0) Found.push_back(Word.substr(0, Comma));
                Word.remove_prefix(Comma == std::string_view::npos ? Word.size() : Comma + 1);
            }
        }
        At = End;
    }
}

}  // namespace hub
