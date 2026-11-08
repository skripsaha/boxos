/* A file read as a book. */

#include "book.h"

#include <algorithm>
#include <format>
#include <optional>

#include "box/cxx/error.h"

#include "glyph.h"

namespace read {

namespace {

constexpr std::size_t TabStop = 4;

/* A line of the file: where it begins, and its text with its tabs as blanks. */
struct SourceLine {
    std::size_t At = 0;
    std::string Text;
};

std::string Expanded(std::string_view Raw)
{
    std::string Out;
    std::size_t Column = 0;
    for (std::size_t Byte = 0; Byte < Raw.size();) {
        if (Raw[Byte] == '\t') {
            const std::size_t Blanks = TabStop - Column % TabStop;
            Out.append(Blanks, ' ');
            Column += Blanks;
            Byte++;
            continue;
        }
        const Glyph Shape = GlyphAt(Raw, Byte);
        Out.append(Raw.substr(Byte, Shape.Length));
        Column += Shape.Width;
        Byte += Shape.Length;
    }
    return Out;
}

std::vector<SourceLine> LinesOf(std::string_view Bytes)
{
    std::vector<SourceLine> Lines;
    std::size_t             At = 0;
    while (At < Bytes.size()) {
        std::size_t End = Bytes.find('\n', At);
        if (End == std::string_view::npos) End = Bytes.size();
        std::string_view Raw = Bytes.substr(At, End - At);
        if (!Raw.empty() && Raw.back() == '\r') Raw.remove_suffix(1);
        Lines.push_back({At, Expanded(Raw)});
        At = End + 1;
    }
    return Lines;
}

std::size_t IndentOf(std::string_view Line) noexcept
{
    std::size_t Blanks = 0;
    while (Blanks < Line.size() && Line[Blanks] == ' ') Blanks++;
    return Blanks;
}

std::string_view Trimmed(std::string_view Text) noexcept
{
    while (!Text.empty() && Text.front() == ' ') Text.remove_prefix(1);
    while (!Text.empty() && Text.back() == ' ') Text.remove_suffix(1);
    return Text;
}

bool Blank(std::string_view Line) noexcept
{
    return Trimmed(Line).empty();
}

bool IsDigit(char Ch) noexcept { return Ch >= '0' && Ch <= '9'; }
bool IsAlnum(char Ch) noexcept
{
    return IsDigit(Ch) || (Ch >= 'a' && Ch <= 'z') || (Ch >= 'A' && Ch <= 'Z') || static_cast<unsigned char>(Ch) >= 0x80;
}
bool IsPunct(char Ch) noexcept
{
    return Ch > 0x20 && Ch < 0x7F && !IsAlnum(Ch);
}

/* The structures a line can begin, each read after at most three blanks. */

/* A fence: three or more backquotes or tildes; *Mark is which. */
bool Fence(std::string_view Line, char *Mark = nullptr) noexcept
{
    if (IndentOf(Line) > 3) return false;
    const std::string_view Rest = Trimmed(Line);
    if (Rest.size() < 3 || (Rest[0] != '`' && Rest[0] != '~') || Rest[1] != Rest[0] || Rest[2] != Rest[0]) return false;
    if (Mark) *Mark = Rest[0];
    return true;
}

/* A heading of #s: its level, and its words in *Words. */
int AtxHeading(std::string_view Line, std::string_view *Words = nullptr) noexcept
{
    if (IndentOf(Line) > 3) return 0;
    std::string_view Rest  = Trimmed(Line);
    int              Level = 0;
    while (Level < static_cast<int>(Rest.size()) && Rest[Level] == '#') Level++;
    if (Level == 0 || Level > 6) return 0;
    if (Level < static_cast<int>(Rest.size()) && Rest[Level] != ' ') return 0;
    Rest.remove_prefix(Level);
    Rest = Trimmed(Rest);
    while (!Rest.empty() && Rest.back() == '#') Rest.remove_suffix(1);
    if (Words) *Words = Trimmed(Rest);
    return Level;
}

/* Three or more of one of - * _, and blanks. */
bool Rule(std::string_view Line) noexcept
{
    if (IndentOf(Line) > 3) return false;
    const std::string_view Rest = Trimmed(Line);
    if (Rest.empty() || (Rest[0] != '-' && Rest[0] != '*' && Rest[0] != '_')) return false;
    std::size_t Marks = 0;
    for (const char Ch : Rest) {
        if (Ch == Rest[0]) Marks++;
        else if (Ch != ' ') return false;
    }
    return Marks >= 3;
}

/* A line of = or of - under a paragraph makes it a heading: 1 or 2. */
int SetextLevel(std::string_view Line) noexcept
{
    if (IndentOf(Line) > 3) return 0;
    const std::string_view Rest = Trimmed(Line);
    if (Rest.size() < 3) return 0;
    if (std::all_of(Rest.begin(), Rest.end(), [](char Ch) { return Ch == '='; })) return 1;
    if (std::all_of(Rest.begin(), Rest.end(), [](char Ch) { return Ch == '-'; })) return 2;
    return 0;
}

bool Quote(std::string_view Line) noexcept
{
    return IndentOf(Line) <= 3 && !Trimmed(Line).empty() && Trimmed(Line)[0] == '>';
}

bool Table(std::string_view Line) noexcept
{
    return !Trimmed(Line).empty() && Trimmed(Line)[0] == '|';
}

/* An item of a list: "- ", "* ", "+ " or a number and "." or ")" and a blank.
 * *Indent is where its marker stands, *Marker the number said (empty for a
 * bullet), *Words where its words begin. */
bool Item(std::string_view Line, std::size_t *Indent = nullptr, std::string *Marker = nullptr,
          std::size_t *Words = nullptr)
{
    const std::size_t At = IndentOf(Line);
    if (At >= Line.size()) return false;
    std::size_t After = 0;
    std::string Said;
    if ((Line[At] == '-' || Line[At] == '*' || Line[At] == '+') && At + 1 < Line.size() && Line[At + 1] == ' ') {
        After = At + 2;
    } else {
        std::size_t End = At;
        while (End < Line.size() && End - At < 9 && IsDigit(Line[End])) End++;
        if (End == At || End + 1 >= Line.size() || (Line[End] != '.' && Line[End] != ')') || Line[End + 1] != ' ')
            return false;
        Said  = std::string(Line.substr(At, End - At)) + ".";
        After = End + 2;
    }
    if (Rule(Line)) return false;
    if (Indent) *Indent = At;
    if (Marker) *Marker = Said;
    if (Words) *Words = After;
    return true;
}

bool Structural(std::string_view Line)
{
    return Fence(Line) || AtxHeading(Line) != 0 || Quote(Line) || Item(Line) || Table(Line) || Rule(Line);
}

std::size_t CountWords(std::string_view Text) noexcept
{
    std::size_t Words  = 0;
    bool        InWord = false;
    for (const char Ch : Text) {
        const bool Space = Ch == ' ' || Ch == '\n';
        if (!Space && !InWord) Words++;
        InWord = !Space;
    }
    return Words;
}

/* ---- the marks inside a line -------------------------------------------- */

void Inline(std::string_view In, Block &Into);

/* In put into the block as it reads, then the whole of it marked Is. The
 * stretches inside it were marked first, so they are the ones found first. */
void Marked(std::string_view In, Look Is, Block &Into)
{
    const std::size_t From = Into.Text.size();
    Inline(In, Into);
    if (Into.Text.size() > From) Into.Looks.push_back({From, Into.Text.size(), Is});
}

/* Where an emphasis opened at At by Mark — one character, or two for strong
 * — is closed, or npos: the closing mark follows no blank, and an underscore
 * stands at no word's middle. */
std::size_t Closing(std::string_view In, std::size_t At, std::string_view Mark)
{
    for (std::size_t Try = At + Mark.size() + 1; Try + Mark.size() <= In.size(); Try++) {
        if (In.substr(Try, Mark.size()) != Mark || In[Try - 1] == ' ') continue;
        if (Mark.size() == 1 && Try + 1 < In.size() && In[Try + 1] == Mark[0]) {
            Try++;
            continue;
        }
        const std::size_t After = Try + Mark.size();
        if (Mark[0] == '_' && After < In.size() && IsAlnum(In[After])) continue;
        return Try;
    }
    return std::string_view::npos;
}

void Inline(std::string_view In, Block &Into)
{
    std::string &Out = Into.Text;
    std::size_t  At  = 0;
    while (At < In.size()) {
        const char Ch = In[At];

        if (Ch == '\\' && At + 1 < In.size() && IsPunct(In[At + 1])) {
            Out += In[At + 1];
            At += 2;
            continue;
        }

        if (Ch == '`') {
            std::size_t Ticks = 0;
            while (At + Ticks < In.size() && In[At + Ticks] == '`') Ticks++;
            const std::size_t Close = In.find(std::string(Ticks, '`'), At + Ticks);
            if (Close != std::string_view::npos) {
                std::string_view Code = In.substr(At + Ticks, Close - At - Ticks);
                if (Code.size() >= 2 && Code.front() == ' ' && Code.back() == ' ') Code = Code.substr(1, Code.size() - 2);
                const std::size_t From = Out.size();
                Out += Code;
                if (Out.size() > From) Into.Looks.push_back({From, Out.size(), Look::Code});
                At = Close + Ticks;
                continue;
            }
        }

        if ((Ch == '*' || Ch == '_') && At + 2 < In.size() && In[At + 1] == Ch && In[At + 2] != ' ') {
            const std::string_view Mark  = In.substr(At, 2);
            const std::size_t      Close = Closing(In, At, Mark);
            if (Close != std::string_view::npos) {
                Marked(In.substr(At + 2, Close - At - 2), Look::Strong, Into);
                At = Close + 2;
                continue;
            }
        }

        if ((Ch == '*' || Ch == '_') && At + 1 < In.size() && In[At + 1] != ' ' &&
            (Ch == '*' || At == 0 || !IsAlnum(In[At - 1]))) {
            const std::size_t Close = Closing(In, At, In.substr(At, 1));
            if (Close != std::string_view::npos) {
                Marked(In.substr(At + 1, Close - At - 1), Look::Emphasis, Into);
                At = Close + 1;
                continue;
            }
        }

        /* [words](where) and ![words](where): the words, as a link. */
        const bool Image = Ch == '!' && At + 1 < In.size() && In[At + 1] == '[';
        if (Ch == '[' || Image) {
            const std::size_t Open  = At + (Image ? 1 : 0);
            const std::size_t Shut  = In.find(']', Open + 1);
            const std::size_t Where = Shut == std::string_view::npos ? Shut : Shut + 1;
            if (Shut != std::string_view::npos && Where < In.size() && In[Where] == '(') {
                const std::size_t End = In.find(')', Where + 1);
                if (End != std::string_view::npos) {
                    Marked(In.substr(Open + 1, Shut - Open - 1), Look::Link, Into);
                    At = End + 1;
                    continue;
                }
            }
        }

        /* <https://...>: the address, as a link. */
        if (Ch == '<') {
            const std::size_t Shut = In.find('>', At + 1);
            if (Shut != std::string_view::npos) {
                const std::string_view Inside = In.substr(At + 1, Shut - At - 1);
                if (Inside.find(' ') == std::string_view::npos &&
                    (Inside.find("://") != std::string_view::npos || Inside.find('@') != std::string_view::npos)) {
                    const std::size_t From = Out.size();
                    Out += Inside;
                    Into.Looks.push_back({From, Out.size(), Look::Link});
                    At = Shut + 1;
                    continue;
                }
            }
        }

        Out += Ch;
        At++;
    }
}

/* ---- prose -------------------------------------------------------------- */

class ProseReader {
public:
    explicit ProseReader(const std::vector<SourceLine> &LinesValue) : Lines(LinesValue) {}

    std::vector<Block> Read()
    {
        while (At < Lines.size()) {
            const std::string_view Line = Lines[At].Text;
            if (Blank(Line))                          At++;
            else if (Fence(Line))                     FencedCode();
            else if (AtxHeading(Line) != 0)           Heading();
            else if (Rule(Line))                      RuleLine();
            else if (Table(Line))                     TableLines();
            else if (Quote(Line))                     QuoteLines();
            else if (Item(Line))                      ItemLines();
            else if (IndentOf(Line) >= 4)             IndentedCode();
            else                                      Paragraph();
        }
        return std::move(Out);
    }

private:
    Block &Begin(Kind Is, std::uint8_t Level = 0)
    {
        Out.push_back({});
        Block &One = Out.back();
        One.Is     = Is;
        One.Level  = Level;
        One.Source = Lines[At].At;
        return One;
    }

    void FencedCode()
    {
        char Mark = '`';
        (void)Fence(Lines[At].Text, &Mark);
        Block &One   = Begin(Kind::Code);
        bool   First = true;
        At++;
        while (At < Lines.size()) {
            char Closing = 0;
            if (Fence(Lines[At].Text, &Closing) && Closing == Mark) {
                At++;
                break;
            }
            if (!First) One.Text += '\n';
            One.Text += Lines[At].Text;
            First = false;
            At++;
        }
    }

    void Heading()
    {
        std::string_view Words;
        const int        Level = AtxHeading(Lines[At].Text, &Words);
        Block           &One   = Begin(Kind::Heading, static_cast<std::uint8_t>(Level));
        Inline(Words, One);
        At++;
    }

    void RuleLine()
    {
        Begin(Kind::Rule);
        At++;
    }

    void TableLines()
    {
        Block &One = Begin(Kind::Table);
        while (At < Lines.size() && Table(Lines[At].Text)) {
            if (!One.Text.empty()) One.Text += '\n';
            One.Text += Trimmed(Lines[At].Text);
            At++;
        }
    }

    /* Lines of >, one quote; a line of > alone ends it and begins the next. */
    void QuoteLines()
    {
        std::string  Words;
        std::uint8_t Depth = 0;
        Block       &One   = Begin(Kind::Quote);
        while (At < Lines.size() && Quote(Lines[At].Text)) {
            std::string_view Rest = Trimmed(Lines[At].Text);
            std::uint8_t     Here = 0;
            while (!Rest.empty() && (Rest.front() == '>' || Rest.front() == ' ')) {
                if (Rest.front() == '>') Here++;
                Rest.remove_prefix(1);
            }
            if (Rest.empty() && !Words.empty()) break;
            if (Depth == 0) Depth = Here;
            if (!Rest.empty()) {
                if (!Words.empty()) Words += ' ';
                Words += Rest;
            }
            At++;
        }
        if (Words.empty()) {
            Out.pop_back();
            return;
        }
        One.Level = Depth;
        Inline(Words, One);
    }

    /* An item and its lines: those after it that begin nothing of their own. */
    void ItemLines()
    {
        std::size_t Indent = 0, Words = 0;
        std::string Marker;
        (void)Item(Lines[At].Text, &Indent, &Marker, &Words);
        Block &One  = Begin(Kind::Item, static_cast<std::uint8_t>(std::min<std::size_t>(Indent / 2, 8)));
        One.Marker  = Marker;
        std::string Said(Trimmed(std::string_view(Lines[At].Text).substr(Words)));
        At++;
        while (At < Lines.size() && !Blank(Lines[At].Text) && !Structural(Lines[At].Text)) {
            Said += ' ';
            Said += Trimmed(Lines[At].Text);
            At++;
        }
        Inline(Said, One);
    }

    /* Lines of four blanks or more; a blank line among them stays, one that
     * ends them does not. */
    void IndentedCode()
    {
        Block      &One = Begin(Kind::Code);
        std::size_t End = At;
        for (std::size_t Look = At; Look < Lines.size(); Look++) {
            if (Blank(Lines[Look].Text)) continue;
            if (IndentOf(Lines[Look].Text) < 4) break;
            End = Look + 1;
        }
        for (bool First = true; At < End; At++, First = false) {
            if (!First) One.Text += '\n';
            const std::string &Line = Lines[At].Text;
            if (Line.size() > 4) One.Text += Line.substr(4);
        }
    }

    /* Lines until a blank one or one that begins something else; a line of =
     * or - under them makes them a heading. */
    void Paragraph()
    {
        Block      &One = Begin(Kind::Paragraph);
        std::string Words(Trimmed(Lines[At].Text));
        At++;
        while (At < Lines.size() && !Blank(Lines[At].Text)) {
            if (const int Level = SetextLevel(Lines[At].Text); Level != 0) {
                One.Is    = Kind::Heading;
                One.Level = static_cast<std::uint8_t>(Level);
                At++;
                break;
            }
            if (Structural(Lines[At].Text)) break;
            Words += ' ';
            Words += Trimmed(Lines[At].Text);
            At++;
        }
        Inline(Words, One);
    }

    const std::vector<SourceLine> &Lines;
    std::size_t                    At = 0;
    std::vector<Block>             Out;
};

}  // namespace

Book Book::Make(file_id_t File, std::string Name, std::string Bytes, Reading As)
{
    Book One;
    One.File  = File;
    One.Name  = std::move(Name);
    One.As    = As;
    One.Bytes = std::move(Bytes);

    const std::vector<SourceLine> Lines = LinesOf(One.Bytes);
    if (As == Reading::Prose) {
        One.Blocks = ProseReader(Lines).Read();
    } else {
        for (const SourceLine &Line : Lines) {
            Block Each;
            Each.Is     = Kind::Line;
            Each.Text   = Line.Text;
            Each.Source = Line.At;
            One.Blocks.push_back(std::move(Each));
        }
    }
    for (Block &Each : One.Blocks) {
        Each.Words = CountWords(Each.Text);
        One.Words += Each.Words;
    }
    return One;
}

std::size_t Book::BlockAt(std::size_t Source) const noexcept
{
    const auto After = std::upper_bound(Blocks.begin(), Blocks.end(), Source,
                                        [](std::size_t Place, const Block &One) { return Place < One.Source; });
    return After == Blocks.begin() ? 0 : static_cast<std::size_t>(After - Blocks.begin()) - 1;
}

std::vector<std::size_t> Book::Headings() const
{
    std::vector<std::size_t> Found;
    for (std::size_t Index = 0; Index < Blocks.size(); Index++)
        if (Blocks[Index].Is == Kind::Heading) Found.push_back(Index);
    return Found;
}

std::size_t Book::WordsFrom(std::size_t From) const noexcept
{
    std::size_t Words = 0;
    for (std::size_t Index = From; Index < Blocks.size(); Index++) Words += Blocks[Index].Words;
    return Words;
}

Reading ReadingFor(std::string_view Name) noexcept
{
    const std::size_t Dot = Name.rfind('.');
    if (Dot == std::string_view::npos || Dot == 0) return Reading::Prose;
    std::string Ending(Name.substr(Dot + 1));
    for (char &Ch : Ending)
        if (Ch >= 'A' && Ch <= 'Z') Ch = static_cast<char>(Ch - 'A' + 'a');
    return Ending == "md" || Ending == "markdown" || Ending == "txt" || Ending == "text" ? Reading::Prose
                                                                                       : Reading::Lines;
}

std::expected<Book, std::string> Open(file_id_t File)
{
    FileMetadata Facts;
    if (const int Answer = FileMetadataRead(File, &Facts); Answer != 0)
        return std::unexpected(std::format("that file would not say what it is: {}",
                                           box::error(box_errno_of(Answer)).message()));
    const std::string   Name = Facts.Filename ? Facts.Filename : "";
    const std::uint64_t Size = Facts.Size;
    FileMetadataRelease(&Facts);

    std::string   Bytes(static_cast<std::size_t>(Size), '\0');
    std::uint64_t Got = 0;
    while (Got < Size) {
        const std::int64_t Read = fread(File, Got, Bytes.data() + Got, static_cast<std::size_t>(Size - Got));
        if (Read < 0)
            return std::unexpected(std::format("{} would not be read past byte {}: {}", Name, Got,
                                               box::error(box_errno_of(static_cast<int>(Read))).message()));
        if (Read == 0) break;
        Got += static_cast<std::uint64_t>(Read);
    }
    Bytes.resize(static_cast<std::size_t>(Got));
    if (Bytes.find('\0') != std::string::npos)
        return std::unexpected(std::format("{} holds bytes that are no text", Name));
    return Book::Make(File, Name, std::move(Bytes), ReadingFor(Name));
}

}  // namespace read
