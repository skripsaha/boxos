/* seek — the files whose text says what is asked.
 *
 *   seek TODO                   the files of the Use Context that say TODO
 *   seek TODO urgent            that say both, anywhere in them
 *   seek "x == 1"               that say it as one phrase
 *   seek TODO them              among what the last command gave
 *   seek TODO everywhere all    on the whole volume
 *
 * Big and small letters are one. Each file found is shown with the lines
 * that say what was asked, the words lit, and the files found are given
 * back: `edit them` opens them. A file that holds no text — a program, a
 * picture — is passed over and counted. A file is read a piece at a time,
 * so its length asks no memory; a line is held whole while it is looked at.
 */

#include <algorithm>
#include <format>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "box/color.h"
#include "box/display.h"
#include "box/file.h"
#include "box/form.h"
#include "box/memory.h"
#include "box/print.h"
#include "box/use.h"

BOX_FORM("takes words \"what to look for: every word somewhere in the file; a phrase in quotes\"\n"
         "takes optional things \"where to look: files, them, everywhere all; the Use Context if none\"\n"
         "gives things\n");

namespace {

constexpr std::size_t ChunkBytes = 64 * 1024;
constexpr std::size_t TabWidth   = 4;
/* How much of a long line is shown before its first lit word, on the glass. */
constexpr std::size_t LeadCells  = 24;

constexpr Color NameInk   = COLOR_TEAL;
constexpr Color NumberInk = COLOR_SLATE;
constexpr Color LitInk    = COLOR_BLACK;
constexpr Color LitPaper  = COLOR_AMBER;

/* Bytes [From, To) of a line, lit. */
struct Lit {
    std::size_t From = 0;
    std::size_t To   = 0;
};

struct Line {
    std::uint64_t    Number = 0;
    std::string      Text;
    std::vector<Lit> Lits;
};

enum class Verdict { Says, SaysNot, NoText, Unread };

char Fold(char Ch) noexcept
{
    return Ch >= 'A' && Ch <= 'Z' ? static_cast<char>(Ch - 'A' + 'a') : Ch;
}

std::string Folded(std::string_view Text)
{
    std::string Out(Text);
    for (char &Ch : Out) Ch = Fold(Ch);
    return Out;
}

void Print(std::string_view Text, Color Ink = COLOR_DEFAULT, Color Paper = COLOR_DEFAULT)
{
    if (Text.empty()) return;
    set_color(Ink);
    set_color_bg(Paper);
    print_bytes(Text.data(), Text.size());
    set_color(COLOR_DEFAULT);
    set_color_bg(COLOR_DEFAULT);
}

/* The width of the glass in cells; 0 under an arrow, where lines go whole. */
std::size_t GlassWidth()
{
    if (form_screen_file() != 0) return 0;
    std::uint32_t Cols = 0, Rows = 0;
    if (display_size(&Cols, &Rows) != 0 || Cols == 0) return 80;
    return Cols;
}

/* A byte that begins a character: UTF-8's continuation bytes do not. */
bool Begins(char Ch) noexcept
{
    return (static_cast<unsigned char>(Ch) & 0xC0) != 0x80;
}

/* What the words are looked for with, and what was learned of each file. */
class Seeker {
public:
    explicit Seeker(const std::vector<std::string> &Words)
    {
        for (const std::string &Word : Words)
            if (!Word.empty()) Wanted.push_back(Folded(Word));
    }

    bool Empty() const noexcept { return Wanted.empty(); }

    /* Reads the file through, keeping the lines that say any word; Says only
     * when every word was found somewhere in it. */
    Verdict Scan(file_id_t File, std::uint64_t Size, std::vector<Line> &Lines)
    {
        Lines.clear();
        Seen = std::vector<bool>(Wanted.size(), false);
        Held.clear();
        Number = 1;

        std::string   Chunk(ChunkBytes, '\0');
        std::uint64_t Offset = 0;
        while (Offset < Size) {
            const std::int64_t Got = fread(File, Offset, Chunk.data(), Chunk.size());
            if (Got < 0) return Verdict::Unread;
            if (Got == 0) break;
            const std::string_view Piece(Chunk.data(), static_cast<std::size_t>(Got));
            /* A zero byte is no text anybody wrote. */
            if (Piece.find('\0') != std::string_view::npos) return Verdict::NoText;
            Take(Piece, Lines);
            Offset += static_cast<std::uint64_t>(Got);
        }
        if (!Held.empty()) Finish(Lines);
        return std::all_of(Seen.begin(), Seen.end(), [](bool One) { return One; }) ? Verdict::Says
                                                                                    : Verdict::SaysNot;
    }

private:
    void Take(std::string_view Piece, std::vector<Line> &Lines)
    {
        std::size_t At = 0;
        while (At < Piece.size()) {
            const std::size_t End = Piece.find('\n', At);
            if (End == std::string_view::npos) {
                Held.append(Piece.substr(At));
                return;
            }
            Held.append(Piece.substr(At, End - At));
            Finish(Lines);
            At = End + 1;
        }
    }

    /* The held line looked at, kept when it says a word, and let go. */
    void Finish(std::vector<Line> &Lines)
    {
        if (!Held.empty() && Held.back() == '\r') Held.pop_back();
        const std::string Low = Folded(Held);
        std::vector<Lit>  Lits;
        for (std::size_t Index = 0; Index < Wanted.size(); Index++) {
            const std::string &Word = Wanted[Index];
            for (std::size_t At = Low.find(Word); At != std::string::npos; At = Low.find(Word, At + Word.size())) {
                Lits.push_back({At, At + Word.size()});
                Seen[Index] = true;
            }
        }
        if (!Lits.empty()) Lines.push_back({Number, std::move(Held), Merged(std::move(Lits))});
        Held.clear();
        Number++;
    }

    static std::vector<Lit> Merged(std::vector<Lit> Lits)
    {
        std::sort(Lits.begin(), Lits.end(), [](const Lit &A, const Lit &B) { return A.From < B.From; });
        std::vector<Lit> Out;
        for (const Lit &One : Lits) {
            if (!Out.empty() && One.From <= Out.back().To) Out.back().To = std::max(Out.back().To, One.To);
            else                                           Out.push_back(One);
        }
        return Out;
    }

    std::vector<std::string> Wanted;
    std::vector<bool>        Seen;
    std::string              Held;
    std::uint64_t            Number = 1;
};

/* The cells Text takes from byte From on, a tab to the next stop. */
std::size_t CellsFrom(std::string_view Text, std::size_t From) noexcept
{
    std::size_t Cells = 0;
    for (std::size_t At = From; At < Text.size(); At++)
        if (Begins(Text[At])) Cells += Text[At] == '\t' ? TabWidth - Cells % TabWidth : 1;
    return Cells;
}

/* One line found: its number, then its text with the words lit — on the
 * glass no wider than the glass, begun a little before the first lit word
 * and cut with "..." when the line is longer; under an arrow, whole. The last
 * column of the glass is left free, so a line never runs onto the next. */
void Show(const Line &One, std::size_t Width)
{
    const std::string Number = std::format("{:>6}  ", One.Number);
    Print(Number, NumberInk);

    std::string_view Text = One.Text;
    std::size_t      From = 0;
    std::size_t      Room = Width > Number.size() + 1 ? Width - Number.size() - 1 : 0;
    if (Width != 0 && CellsFrom(Text, 0) > Room && !One.Lits.empty() && One.Lits.front().From > LeadCells) {
        From = One.Lits.front().From - LeadCells;
        while (From < Text.size() && !Begins(Text[From])) From++;
        Print("...");
        Room = Room > 3 ? Room - 3 : 0;
    }
    if (Width != 0 && CellsFrom(Text, From) > Room) Room = Room > 3 ? Room - 3 : 0;

    std::size_t Cells  = 0;
    std::size_t Next   = 0;     /* the first lit run not yet behind */
    std::string Run;
    bool        RunLit = false;
    auto Flush = [&] {
        Print(Run, RunLit ? LitInk : COLOR_DEFAULT, RunLit ? LitPaper : COLOR_DEFAULT);
        Run.clear();
    };
    for (std::size_t At = From; At < Text.size(); At++) {
        while (Next < One.Lits.size() && One.Lits[Next].To <= At) Next++;
        const bool InLit = Next < One.Lits.size() && One.Lits[Next].From <= At;
        if (InLit != RunLit) {
            Flush();
            RunLit = InLit;
        }
        const char  Ch    = Text[At];
        std::size_t Takes = 0;
        if (Begins(Ch)) {
            Takes = Ch == '\t' ? TabWidth - Cells % TabWidth : 1;
            if (Width != 0 && Cells + Takes > Room) {
                Flush();
                Print("...");
                break;
            }
            Cells += Takes;
        }
        if (Ch == '\t')                                 Run.append(Takes, ' ');
        else if (static_cast<unsigned char>(Ch) < 0x20) Run += ' ';
        else                                            Run += Ch;
    }
    Flush();
    Print("\n");
}

/* The Use Context as its tags say it, comma-joined; empty with none. */
std::string ContextTags()
{
    std::size_t Needed = 0;
    if (use_get(nullptr, 0, &Needed) < 0 || Needed <= 1) return {};
    std::string Tags(Needed, '\0');
    if (use_get(Tags.data(), Tags.size(), &Needed) < 0) return {};
    Tags.resize(Tags.find('\0'));
    return Tags;
}

std::string Plural(std::size_t Count, std::string_view One, std::string_view Many)
{
    return std::format("{} {}", Count, Count == 1 ? One : Many);
}

int Seek(const std::vector<std::string> &Words, std::vector<file_id_t> Where, bool Given)
{
    Seeker Look(Words);
    if (Look.Empty()) {
        Print("seek: there is nothing to look for\n");
        return 1;
    }

    std::string Place;
    if (Given) {
        Place = "among " + Plural(Where.size(), "file given", "files given");
    } else {
        file_id_t *Ids   = nullptr;
        const int  Count = query_all(nullptr, nullptr, &Ids);
        if (Count < 0) {
            Print("seek: the volume would not say what files it has\n");
            return 1;
        }
        Where.assign(Ids, Ids + Count);
        free(Ids);
        const std::string Context = ContextTags();
        Place = Context.empty() ? std::string("on the whole volume") : "in the Use Context " + Context;
    }

    const std::size_t      Width = GlassWidth();
    std::vector<file_id_t> Found;
    std::vector<Line>      Lines;
    std::size_t            LineCount = 0, NoText = 0, Unread = 0;
    for (const file_id_t File : Where) {
        FileMetadata Facts;
        if (FileMetadataRead(File, &Facts) != 0) {
            Unread++;
            continue;
        }
        const std::string   Name    = Facts.Filename ? Facts.Filename : "";
        const std::uint64_t Size    = Facts.Size;
        const bool          Trashed = Facts.KeptUntil != 0;
        FileMetadataRelease(&Facts);
        if (Trashed) continue;

        switch (Look.Scan(File, Size, Lines)) {
        case Verdict::NoText:  NoText++; continue;
        case Verdict::Unread:  Unread++; continue;
        case Verdict::SaysNot: continue;
        case Verdict::Says:    break;
        }
        if (!Found.empty()) Print("\n");
        Print(Name + "\n", NameInk);
        for (const Line &One : Lines) Show(One, Width);
        Found.push_back(File);
        LineCount += Lines.size();
    }

    std::string Asked;
    for (const std::string &Word : Words) Asked += (Asked.empty() ? "" : " ") + Word;
    if (!Found.empty()) Print("\n");
    if (Found.empty())
        Print(std::format("seek: nothing {} says {}\n", Place, Asked));
    else
        Print(std::format("seek: {} {} say {}, on {}\n", Plural(Found.size(), "file", "files"), Place, Asked,
                          Plural(LineCount, "line", "lines")));
    if (NoText != 0)
        Print(std::format("seek: {} no text and {} passed over\n", Plural(NoText, "file holds", "files hold"),
                          NoText == 1 ? "was" : "were"));
    if (Unread != 0)
        Print(std::format("seek: {} not be read\n", Plural(Unread, "file could", "files could")));

    souvenir_things(Found.data(), Found.size());
    return 0;
}

}  // namespace

int main()
{
    try {
        const char *const *Words;
        std::size_t        WordCount;
        const file_id_t   *Files;
        std::size_t        FileCount;
        if (form_words(0, &Words, &WordCount) != 0 || form_things(1, &Files, &FileCount) != 0) return 1;
        return Seek(std::vector<std::string>(Words, Words + WordCount),
                    std::vector<file_id_t>(Files, Files + FileCount), FileCount != 0);
    } catch (const std::bad_alloc &) {
        Print("seek: there is no memory to look with\n");
        return 1;
    }
}
