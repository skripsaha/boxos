/* The lines of a file: putting bytes in and taking them out, walking by
 * characters and words, columns, and finding. */

#include "text.h"

#include <algorithm>
#include <iterator>

namespace edit {

namespace {

char Lower(char Ch) noexcept
{
    return Ch >= 'A' && Ch <= 'Z' ? static_cast<char>(Ch - 'A' + 'a') : Ch;
}

bool SameAt(std::string_view Hay, std::size_t At, std::string_view Needle, bool Folded) noexcept
{
    for (std::size_t Index = 0; Index < Needle.size(); Index++) {
        const char Have = Hay[At + Index];
        if (Folded ? Lower(Have) != Lower(Needle[Index]) : Have != Needle[Index]) return false;
    }
    return true;
}

/* The first place at or after From where Needle stands in Hay. */
std::optional<std::size_t> FirstIn(std::string_view Hay, std::string_view Needle, std::size_t From,
                                   bool Folded) noexcept
{
    if (Needle.size() > Hay.size()) return std::nullopt;
    for (std::size_t At = From; At + Needle.size() <= Hay.size(); At++)
        if (SameAt(Hay, At, Needle, Folded)) return At;
    return std::nullopt;
}

/* The last place before Until where Needle starts in Hay. */
std::optional<std::size_t> LastIn(std::string_view Hay, std::string_view Needle, std::size_t Until,
                                  bool Folded) noexcept
{
    if (Needle.size() > Hay.size()) return std::nullopt;
    std::size_t At = std::min(Until, Hay.size() - Needle.size() + 1);
    while (At > 0) {
        At--;
        if (SameAt(Hay, At, Needle, Folded)) return At;
    }
    return std::nullopt;
}

}  // namespace

Position After(Position At, std::string_view Bytes) noexcept
{
    const std::size_t Last = Bytes.rfind('\n');
    if (Last == std::string_view::npos) return {At.Line, At.Byte + Bytes.size()};
    return {At.Line + static_cast<std::size_t>(std::count(Bytes.begin(), Bytes.end(), '\n')),
            Bytes.size() - Last - 1};
}

Kind KindAt(std::string_view Line, std::size_t Byte) noexcept
{
    const auto Ch = static_cast<unsigned char>(Line[Byte]);
    if (Ch == ' ' || Ch == '\t') return Kind::Blank;
    if (Ch >= 0x80 || Ch == '_' || (Ch >= '0' && Ch <= '9') || (Ch >= 'a' && Ch <= 'z') || (Ch >= 'A' && Ch <= 'Z'))
        return Kind::Word;
    return Kind::Mark;
}

Text Text::FromBytes(std::string_view Bytes)
{
    Text Out;
    Out.Lines.clear();

    const auto Newlines = static_cast<std::size_t>(std::count(Bytes.begin(), Bytes.end(), '\n'));
    std::size_t Crlfs = 0;
    for (std::size_t At = 1; At < Bytes.size(); At++)
        if (Bytes[At] == '\n' && Bytes[At - 1] == '\r') Crlfs++;
    Out.CarriageReturns = Newlines > 0 && Crlfs == Newlines;

    std::size_t Start = 0;
    for (;;) {
        const std::size_t Stop = Bytes.find('\n', Start);
        if (Stop == std::string_view::npos) {
            Out.Lines.emplace_back(Bytes.substr(Start));
            break;
        }
        const std::size_t Cut = Out.CarriageReturns ? Stop - 1 : Stop;
        Out.Lines.emplace_back(Bytes.substr(Start, Cut - Start));
        Start = Stop + 1;
    }
    return Out;
}

std::string Text::ToBytes() const
{
    const std::string_view Break = CarriageReturns ? "\r\n" : "\n";
    std::string            Out;
    Out.reserve(static_cast<std::size_t>(ByteCount()));
    for (std::size_t Index = 0; Index < Lines.size(); Index++) {
        if (Index > 0) Out += Break;
        Out += Lines[Index];
    }
    return Out;
}

std::uint64_t Text::ByteCount() const noexcept
{
    std::uint64_t Count = (Lines.size() - 1) * (CarriageReturns ? 2u : 1u);
    for (const std::string &One : Lines) Count += One.size();
    return Count;
}

Position Text::End() const noexcept
{
    return {Lines.size() - 1, Lines.back().size()};
}

Position Text::Clamp(Position At) const noexcept
{
    if (At.Line >= Lines.size()) return End();
    At.Byte = std::min(At.Byte, Lines[At.Line].size());
    return At;
}

Position Text::Insert(Position At, std::string_view Bytes)
{
    At = Clamp(At);
    Changes++;

    const std::size_t First = Bytes.find('\n');
    if (First == std::string_view::npos) {
        Lines[At.Line].insert(At.Byte, Bytes);
        return {At.Line, At.Byte + Bytes.size()};
    }

    std::string Tail = Lines[At.Line].substr(At.Byte);
    Lines[At.Line].erase(At.Byte);
    Lines[At.Line].append(Bytes.substr(0, First));

    std::vector<std::string> Fresh;
    std::size_t              Start = First + 1;
    for (;;) {
        const std::size_t Stop = Bytes.find('\n', Start);
        if (Stop == std::string_view::npos) {
            Fresh.emplace_back(Bytes.substr(Start));
            break;
        }
        Fresh.emplace_back(Bytes.substr(Start, Stop - Start));
        Start = Stop + 1;
    }
    const Position Ends{At.Line + Fresh.size(), Fresh.back().size()};
    Fresh.back() += Tail;
    Lines.insert(Lines.begin() + static_cast<std::ptrdiff_t>(At.Line + 1), std::make_move_iterator(Fresh.begin()),
                 std::make_move_iterator(Fresh.end()));
    return Ends;
}

std::string Text::Slice(Position From, Position To) const
{
    From = Clamp(From);
    To   = Clamp(To);
    if (To <= From) return {};
    if (From.Line == To.Line) return Lines[From.Line].substr(From.Byte, To.Byte - From.Byte);

    std::string Out = Lines[From.Line].substr(From.Byte);
    for (std::size_t Index = From.Line + 1; Index < To.Line; Index++) {
        Out += '\n';
        Out += Lines[Index];
    }
    Out += '\n';
    Out.append(Lines[To.Line], 0, To.Byte);
    return Out;
}

std::string Text::Erase(Position From, Position To)
{
    From = Clamp(From);
    To   = Clamp(To);
    if (To <= From) return {};
    std::string Taken = Slice(From, To);
    Changes++;

    if (From.Line == To.Line) {
        Lines[From.Line].erase(From.Byte, To.Byte - From.Byte);
        return Taken;
    }
    Lines[From.Line].erase(From.Byte);
    Lines[From.Line].append(Lines[To.Line], To.Byte);
    Lines.erase(Lines.begin() + static_cast<std::ptrdiff_t>(From.Line + 1),
                Lines.begin() + static_cast<std::ptrdiff_t>(To.Line + 1));
    return Taken;
}

Position Text::Next(Position At) const noexcept
{
    At = Clamp(At);
    const std::string &One = Lines[At.Line];
    if (At.Byte < One.size()) return {At.Line, At.Byte + tui::GlyphAt(One, At.Byte, 0).Length};
    if (At.Line + 1 < Lines.size()) return {At.Line + 1, 0};
    return At;
}

Position Text::Prior(Position At) const noexcept
{
    At = Clamp(At);
    if (At.Byte == 0) return At.Line == 0 ? At : Position{At.Line - 1, Lines[At.Line - 1].size()};

    /* Back to the start of the character the caret stands after: walk the
     * line from its start, since only the start knows where characters begin
     * when bytes that are no UTF-8 sit among them. */
    const std::string &One = Lines[At.Line];
    std::size_t        Byte = 0, Before = 0;
    while (Byte < At.Byte) {
        Before = Byte;
        Byte += tui::GlyphAt(One, Byte, 0).Length;
    }
    return {At.Line, Before};
}

std::size_t Text::ColumnOf(Position At) const noexcept
{
    At = Clamp(At);
    const std::string &One = Lines[At.Line];
    std::size_t        Column = 0;
    for (std::size_t Byte = 0; Byte < At.Byte;) {
        const tui::Glyph Shape = tui::GlyphAt(One, Byte, Column);
        Column += Shape.Width;
        Byte += Shape.Length;
    }
    return Column;
}

std::size_t Text::ByteAtColumn(std::size_t Line, std::size_t Column) const noexcept
{
    const std::string &One = Lines[std::min(Line, Lines.size() - 1)];
    std::size_t        At = 0, Byte = 0;
    while (Byte < One.size()) {
        const tui::Glyph Shape = tui::GlyphAt(One, Byte, At);
        if (At + Shape.Width > Column) break;
        At += Shape.Width;
        Byte += Shape.Length;
    }
    return Byte;
}

std::size_t Text::Width(std::size_t Line) const noexcept
{
    return ColumnOf({Line, Lines[Line].size()});
}

std::size_t Text::IndentOf(std::size_t Line) const noexcept
{
    const std::string &One = Lines[Line];
    std::size_t        Byte = 0;
    while (Byte < One.size() && (One[Byte] == ' ' || One[Byte] == '\t')) Byte++;
    return Byte;
}

Position Text::WordAfter(Position At) const noexcept
{
    At = Clamp(At);
    const std::string &One = Lines[At.Line];
    if (At.Byte == One.size()) return Next(At);

    const Kind First = KindAt(One, At.Byte);
    if (First != Kind::Blank)
        while (At.Byte < One.size() && KindAt(One, At.Byte) == First) At = Next(At);
    while (At.Byte < One.size() && KindAt(One, At.Byte) == Kind::Blank) At = Next(At);
    return At;
}

Position Text::WordBefore(Position At) const noexcept
{
    At = Clamp(At);
    if (At.Byte == 0) return Prior(At);

    const std::string &One = Lines[At.Line];
    while (At.Byte > 0 && KindAt(One, Prior(At).Byte) == Kind::Blank) At = Prior(At);
    if (At.Byte == 0) return At;
    const Kind Last = KindAt(One, Prior(At).Byte);
    while (At.Byte > 0 && KindAt(One, Prior(At).Byte) == Last) At = Prior(At);
    return At;
}

std::optional<Position> Text::Find(std::string_view Needle, Position At, bool Forward, bool Folded) const
{
    if (Needle.empty()) return std::nullopt;
    At = Clamp(At);
    const std::size_t Count = Lines.size();

    /* Every line once, starting with the caret's, and the caret's line a
     * second time for what lies on the far side of the caret. */
    for (std::size_t Step = 0; Step <= Count; Step++) {
        const std::size_t Line  = Forward ? (At.Line + Step) % Count : (At.Line + Count - Step % Count) % Count;
        const bool        First = Step == 0, Last = Step == Count;
        std::optional<std::size_t> Found;
        if (Forward) {
            Found = FirstIn(Lines[Line], Needle, First ? At.Byte : 0, Folded);
            if (Found && Last && *Found >= At.Byte) Found.reset();
        } else {
            Found = LastIn(Lines[Line], Needle, First ? At.Byte : Lines[Line].size() + 1, Folded);
            if (Found && Last && *Found < At.Byte) Found.reset();
        }
        if (Found) return Position{Line, *Found};
    }
    return std::nullopt;
}

std::vector<Position> Text::FindAll(std::string_view Needle, bool Folded) const
{
    std::vector<Position> Out;
    if (Needle.empty()) return Out;
    for (std::size_t Line = 0; Line < Lines.size(); Line++) {
        std::size_t From = 0;
        while (std::optional<std::size_t> Found = FirstIn(Lines[Line], Needle, From, Folded)) {
            Out.push_back({Line, *Found});
            From = *Found + Needle.size();
        }
    }
    return Out;
}

}  // namespace edit
