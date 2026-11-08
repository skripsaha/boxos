/* The glass and the arrow's text of difference.
 *
 * Colour is set before each run and rides with it; under an arrow the runs go
 * to the file with their colours dropped, so nothing here asks whether there
 * is an arrow except where the screen's width matters: on the screen a long
 * line is cut to the width and marked, in the file it is written whole.
 */

#include <algorithm>
#include <format>
#include <string>
#include <vector>

#include "box/color.h"
#include "box/display.h"
#include "box/form.h"
#include "box/print.h"

#include "difference.h"

namespace {

constexpr std::size_t Context        = 3;
constexpr std::size_t TabWidth       = 4;
constexpr std::size_t LeastTextWidth = 12;
constexpr std::size_t BytesARow      = 16;

/* The width of a hex row with its text on the right: marker, offset, bytes. */
constexpr std::size_t HexRowWithText = 2 + 8 + 2 + 3 * BytesARow + 1 + 2 + BytesARow;

struct Run {
    std::string Text;
    Color       Fg;
    Color       Bg;
};

/* One line of output, put on the glass in as few runs as it has colours. */
class Row {
public:
    void Add(std::string_view Text, Color Fg, Color Bg = COLOR_DEFAULT)
    {
        if (Text.empty()) return;
        if (!Runs.empty() && Runs.back().Fg == Fg && Runs.back().Bg == Bg) Runs.back().Text += Text;
        else                                                              Runs.push_back({std::string(Text), Fg, Bg});
    }

    void Print()
    {
        if (Runs.empty()) Runs.push_back({std::string(), COLOR_DEFAULT, COLOR_DEFAULT});
        Runs.back().Text += '\n';
        for (const Run &Piece : Runs) {
            set_color(Piece.Fg);
            set_color_bg(Piece.Bg);
            print_bytes(Piece.Text.data(), Piece.Text.size());
        }
        set_color(COLOR_DEFAULT);
        set_color_bg(COLOR_DEFAULT);
        Runs.clear();
    }

private:
    std::vector<Run> Runs;
};

void SayLine(std::string_view Text, Color Fg = COLOR_DEFAULT)
{
    Row Line;
    Line.Add(Text, Fg);
    Line.Print();
}

/* The width of the glass in columns; 0 under an arrow, where lines are whole. */
std::size_t GlassWidth()
{
    if (form_screen_file() != 0) return 0;
    std::uint32_t Cols = 0, Rows = 0;
    if (display_size(&Cols, &Rows) != 0 || Cols == 0) return 80;
    return Cols;
}

bool IsContinuation(char Byte)
{
    return (static_cast<std::uint8_t>(Byte) & 0xc0) == 0x80;
}

/* Text as the glass will show it, up to Budget columns from Column on: a tab
 * fills to the next stop of four, any other control is a '?'. The bytes of a
 * character after its first add no column and are never cut from it. */
std::string Display(std::string_view Text, std::size_t &Column, std::size_t Budget)
{
    std::string Shown;
    for (char Byte : Text) {
        if (IsContinuation(Byte)) {
            Shown += Byte;
            continue;
        }
        if (Column >= Budget) break;
        std::uint8_t Code = static_cast<std::uint8_t>(Byte);
        if (Code == '\t') {
            std::size_t Fill = std::min(TabWidth - Column % TabWidth, Budget - Column);
            Shown.append(Fill, ' ');
            Column += Fill;
        } else if (Code < 0x20 || Code == 0x7f) {
            Shown += '?';
            Column++;
        } else {
            Shown += Byte;
            Column++;
        }
    }
    return Shown;
}

std::size_t Digits(std::size_t Number)
{
    std::size_t Count = 1;
    while (Number >= 10) {
        Number /= 10;
        Count++;
    }
    return Count;
}

std::string Padded(std::size_t Number, std::size_t Width)
{
    std::string Text = std::to_string(Number);
    return Text.size() >= Width ? Text : std::string(Width - Text.size(), ' ') + Text;
}

std::string Plural(std::size_t Count, std::string_view Thing)
{
    return std::format("{} {}{}", Count, Thing, Count == 1 ? "" : "s");
}

/* "lines 12-15 of a.txt", and without the first word when it was said before. */
std::string Phrase(std::size_t Begin, std::size_t Count, std::string_view Label, bool Word)
{
    if (Count == 0) return std::format("no lines of {}", Label);
    if (Count == 1) return std::format("{}{} of {}", Word ? "line " : "", Begin + 1, Label);
    return std::format("{}{}-{} of {}", Word ? "lines " : "", Begin + 1, Begin + Count, Label);
}

enum class Kind { Same, Out, In };

class Painter {
public:
    Painter(const TextFile &First, const TextFile &Second)
        : First(First), Second(Second), Width(Digits(std::max(First.Lines.size(), Second.Lines.size())))
    {
        std::size_t Glass  = GlassWidth();
        std::size_t Gutter = 2 * Width + 4;
        Limit = Glass == 0 ? 0 : (Glass > Gutter + LeastTextWidth ? Glass - 1 - Gutter : LeastTextWidth);
    }

    DifferenceTally Show(const DifferenceMarks &Marks);

private:
    void Names();
    void Hunk(const std::vector<DifferenceBlock> &Blocks, const DifferenceHunk &Which);
    void Header(std::size_t FirstBegin, std::size_t FirstCount, std::size_t SecondBegin, std::size_t SecondCount);
    void Changed(const DifferenceBlock &Block);
    void Unchanged(std::size_t FirstAt, std::size_t SecondAt, std::size_t Count);
    void Line(Kind Which, std::size_t FirstAt, std::size_t SecondAt, std::string_view Text, const DifferenceSpan *Strong);
    void EndNote(const TextFile &File, std::size_t Index);
    void Body(Row &Out, std::string_view Text, const DifferenceSpan *Strong, Color Base, Color StrongBg);

    const TextFile &First;
    const TextFile &Second;
    std::size_t     Width;    /* of a line number */
    std::size_t     Limit;    /* columns of a line's text; 0 for no limit */
    DifferenceTally Tally;
};

void Painter::Names()
{
    Row Taken;
    Taken.Add("- ", COLOR_RED);
    Taken.Add(First.Label, COLOR_RED);
    Taken.Add(std::format("  {}", First.Lines.empty() ? std::string("no lines") : Plural(First.Lines.size(), "line")), COLOR_DARK_GRAY);
    Taken.Print();
    Row Put;
    Put.Add("+ ", COLOR_GREEN);
    Put.Add(Second.Label, COLOR_GREEN);
    Put.Add(std::format("  {}", Second.Lines.empty() ? std::string("no lines") : Plural(Second.Lines.size(), "line")), COLOR_DARK_GRAY);
    Put.Print();
}

DifferenceTally Painter::Show(const DifferenceMarks &Marks)
{
    std::vector<DifferenceBlock> Blocks = DifferenceBlocks(Marks);
    Names();
    for (const DifferenceHunk &Which : DifferenceHunks(Blocks, Context)) {
        SayLine("");
        Hunk(Blocks, Which);
    }
    for (const DifferenceBlock &Block : Blocks) {
        Tally.TakenOut += Block.FirstCount;
        Tally.PutIn += Block.SecondCount;
    }
    Tally.Places = Blocks.size();
    return Tally;
}

void Painter::Header(std::size_t FirstBegin, std::size_t FirstCount, std::size_t SecondBegin, std::size_t SecondCount)
{
    SayLine(std::format("{}, {}", Phrase(FirstBegin, FirstCount, First.Label, true),
                        Phrase(SecondBegin, SecondCount, Second.Label, false)),
            COLOR_CYAN);
}

void Painter::Hunk(const std::vector<DifferenceBlock> &Blocks, const DifferenceHunk &Which)
{
    const DifferenceBlock &Head = Blocks[Which.Begin];
    const DifferenceBlock &Tail = Blocks[Which.End - 1];
    std::size_t            Lead  = std::min(Context, Head.FirstStart);
    std::size_t            Trail = std::min(Context, First.Lines.size() - Tail.FirstEnd());

    std::size_t FirstFrom  = Head.FirstStart - Lead;
    std::size_t SecondFrom = Head.SecondStart - Lead;
    Header(FirstFrom, Tail.FirstEnd() + Trail - FirstFrom, SecondFrom, Tail.SecondEnd() + Trail - SecondFrom);

    Unchanged(FirstFrom, SecondFrom, Lead);
    for (std::size_t I = Which.Begin; I < Which.End; I++) {
        Changed(Blocks[I]);
        std::size_t Gap = I + 1 < Which.End ? Blocks[I + 1].FirstStart - Blocks[I].FirstEnd() : Trail;
        Unchanged(Blocks[I].FirstEnd(), Blocks[I].SecondEnd(), Gap);
    }
}

void Painter::Unchanged(std::size_t FirstAt, std::size_t SecondAt, std::size_t Count)
{
    for (std::size_t I = 0; I < Count; I++)
        Line(Kind::Same, FirstAt + I, SecondAt + I, First.Lines[FirstAt + I], nullptr);
}

/* Where as many lines were put in as were taken out, each pair is one line
 * that changed: what changed inside it is shown in a stronger colour. */
void Painter::Changed(const DifferenceBlock &Block)
{
    bool Paired = Block.FirstCount == Block.SecondCount;
    for (std::size_t I = 0; I < Block.FirstCount; I++) {
        std::string_view Old = First.Lines[Block.FirstStart + I];
        DifferenceSpan   InOld, InNew;
        bool             Strong = Paired && DifferenceInnerSpan(Old, Second.Lines[Block.SecondStart + I], InOld, InNew);
        Line(Kind::Out, Block.FirstStart + I, 0, Old, Strong ? &InOld : nullptr);
    }
    for (std::size_t I = 0; I < Block.SecondCount; I++) {
        std::string_view New = Second.Lines[Block.SecondStart + I];
        DifferenceSpan   InOld, InNew;
        bool             Strong = Paired && DifferenceInnerSpan(First.Lines[Block.FirstStart + I], New, InOld, InNew);
        Line(Kind::In, 0, Block.SecondStart + I, New, Strong ? &InNew : nullptr);
    }
}

/* The line's text in Base colour, the Strong span in white on StrongBg; cut
 * to Limit columns with a mark when it is longer. */
void Painter::Body(Row &Out, std::string_view Text, const DifferenceSpan *Strong, Color Base, Color StrongBg)
{
    std::size_t Begin = Strong ? Strong->Begin : Text.size();
    std::size_t End   = Strong ? Strong->End : Text.size();
    std::string_view Pieces[3] = {Text.substr(0, Begin), Text.substr(Begin, End - Begin), Text.substr(End)};

    if (Limit == 0) {
        Out.Add(Pieces[0], Base);
        Out.Add(Pieces[1], COLOR_WHITE, StrongBg);
        Out.Add(Pieces[2], Base);
        return;
    }

    std::size_t Whole = 0;
    Display(Text, Whole, SIZE_MAX);
    bool        Cut    = Whole > Limit;
    std::size_t Budget = Cut ? Limit - 1 : Limit;

    std::size_t Column = 0;
    Out.Add(Display(Pieces[0], Column, Budget), Base);
    Out.Add(Display(Pieces[1], Column, Budget), COLOR_WHITE, StrongBg);
    Out.Add(Display(Pieces[2], Column, Budget), Base);
    if (Cut) {
        Out.Add(">", COLOR_AMBER);
        Tally.Cut++;
    }
}

void Painter::Line(Kind Which, std::size_t FirstAt, std::size_t SecondAt, std::string_view Text, const DifferenceSpan *Strong)
{
    std::string Blank(Width, ' ');
    Row         Out;
    Out.Add((Which == Kind::In ? Blank : Padded(FirstAt + 1, Width)) + " " +
                (Which == Kind::Out ? Blank : Padded(SecondAt + 1, Width)) + " ",
            COLOR_DARK_GRAY);
    switch (Which) {
    case Kind::Same:
        Out.Add("  ", COLOR_DEFAULT);
        Body(Out, Text, nullptr, COLOR_DEFAULT, COLOR_DEFAULT);
        break;
    case Kind::Out:
        Out.Add("- ", COLOR_RED);
        Body(Out, Text, Strong, COLOR_RED, COLOR_DARK_RED);
        break;
    case Kind::In:
        Out.Add("+ ", COLOR_GREEN);
        Body(Out, Text, Strong, COLOR_GREEN, COLOR_DARK_GREEN);
        break;
    }
    Out.Print();

    if (Which != Kind::In) EndNote(First, FirstAt);
    if (Which != Kind::Out) EndNote(Second, SecondAt);
}

/* A last line with no end is said, not shown as a change. */
void Painter::EndNote(const TextFile &File, std::size_t Index)
{
    if (!File.Facts.Unterminated || Index + 1 != File.Lines.size()) return;
    Row Note;
    Note.Add(std::string(2 * Width + 4, ' '), COLOR_DEFAULT);
    Note.Add(std::format("\\ {} has no newline at its end", File.Label), COLOR_DARK_GRAY);
    Note.Print();
}

bool StylesDiffer(const TextFacts &First, const TextFacts &Second)
{
    return (First.CrLf > 0) != (Second.CrLf > 0) || (First.Lf > 0) != (Second.Lf > 0);
}

std::string StyleSentence(const TextFile &File)
{
    std::string_view Style = DifferenceEndStyle(File.Facts);
    if (File.Facts.CrLf == 0 && File.Facts.Lf == 0) return std::format("{} has {}", File.Label, Style);
    return std::format("{} ends its lines with {}", File.Label, Style);
}

/* How the two files end their lines and their last line, said once. */
void SayEnds(const TextFile &First, const TextFile &Second)
{
    bool Both = (First.Facts.CrLf + First.Facts.Lf > 0) && (Second.Facts.CrLf + Second.Facts.Lf > 0);
    if (Both && StylesDiffer(First.Facts, Second.Facts))
        SayLine(std::format("{}, {}", StyleSentence(First), StyleSentence(Second)), COLOR_DARK_GRAY);
    for (const TextFile *File : {&First, &Second}) {
        const TextFile &Other = File == &First ? Second : First;
        if (File->Facts.Unterminated && !Other.Facts.Unterminated && !Other.Lines.empty())
            SayLine(std::format("{} has no newline at its end", File->Label), COLOR_DARK_GRAY);
    }
}

}

DifferenceTally DifferenceShowText(const TextFile &First, const TextFile &Second, const DifferenceMarks &Marks)
{
    Painter Paint(First, Second);
    return Paint.Show(Marks);
}

void DifferenceSayTextEnd(const TextFile &First, const TextFile &Second, const DifferenceTally &Tally)
{
    SayLine("");
    std::string Taken = Tally.TakenOut == 0 ? std::string("nothing taken out") : Plural(Tally.TakenOut, "line") + " taken out";
    std::string Put   = Tally.PutIn == 0 ? std::string("nothing put in") : Plural(Tally.PutIn, "line") + " put in";
    SayLine(std::format("{}, {}, in {}", Taken, Put, Plural(Tally.Places, "place")));
    SayEnds(First, Second);
    if (Tally.Cut > 0)
        SayLine(std::format("{} cut to the width of the screen; an arrow writes them whole: difference {} {} -> NAME",
                            Plural(Tally.Cut, "line"), First.Label, Second.Label),
                COLOR_DARK_GRAY);
}

void DifferenceSayEndsOnly(const TextFile &First, const TextFile &Second)
{
    SayLine(std::format("{} and {} have the same lines; they differ only in how the lines end", First.Label, Second.Label));
    SayEnds(First, Second);
}

void DifferenceSaySame(std::string_view FirstLabel, std::string_view SecondLabel, std::uint64_t Size)
{
    if (Size == 0) SayLine(std::format("{} and {} are the same, both empty", FirstLabel, SecondLabel));
    else           SayLine(std::format("{} and {} are the same, {}, byte for byte", FirstLabel, SecondLabel, Plural(Size, "byte")));
}

namespace {

/* One file's row of the hex window: Bytes from Base on, those that are not
 * the other file's in a stronger colour. */
void HexRow(std::string_view Marker, Color Hue, Color HueBg, std::string_view Mine, std::string_view Other,
            std::size_t Base, bool WithText)
{
    Row Out;
    Out.Add(Marker, Hue);
    Out.Add(" ", COLOR_DEFAULT);
    Out.Add(std::format("{:08x}  ", Base), COLOR_DARK_GRAY);
    std::string Text;
    for (std::size_t I = 0; I < BytesARow; I++) {
        std::size_t At     = Base + I;
        bool        Here   = At < Mine.size();
        bool        Differs = Here != (At < Other.size()) || (Here && Mine[At] != Other[At]);
        if (Here) {
            Out.Add(std::format("{:02x}", static_cast<std::uint8_t>(Mine[At])), Differs ? COLOR_WHITE : COLOR_DEFAULT,
                    Differs ? HueBg : COLOR_DEFAULT);
            std::uint8_t Code = static_cast<std::uint8_t>(Mine[At]);
            Text += Code >= 0x20 && Code < 0x7f ? static_cast<char>(Code) : '.';
        } else {
            Out.Add("  ", COLOR_DEFAULT);
            Text += ' ';
        }
        Out.Add(I + 1 == BytesARow ? "" : (I == 7 ? "  " : " "), COLOR_DEFAULT);
    }
    if (WithText) {
        Out.Add("  ", COLOR_DEFAULT);
        Out.Add(Text, COLOR_DARK_GRAY);
    }
    Out.Print();
}

}

void DifferenceShowBytes(std::string_view FirstLabel, std::string_view FirstBytes, std::string_view SecondLabel,
                         std::string_view SecondBytes, const BinaryReport &Report)
{
    {
        Row Taken;
        Taken.Add(std::format("- {}", FirstLabel), COLOR_RED);
        Taken.Add(std::format("  {}", Plural(FirstBytes.size(), "byte")), COLOR_DARK_GRAY);
        Taken.Print();
        Row Put;
        Put.Add(std::format("+ {}", SecondLabel), COLOR_GREEN);
        Put.Add(std::format("  {}", Plural(SecondBytes.size(), "byte")), COLOR_DARK_GRAY);
        Put.Print();
    }
    SayLine("");

    if (Report.Differing == 0) {
        std::string_view Longer     = FirstBytes.size() > SecondBytes.size() ? FirstLabel : SecondLabel;
        std::size_t      More       = std::max(FirstBytes.size(), SecondBytes.size()) - Report.Common;
        SayLine(std::format("These are bytes, not lines. The two are the same for their first {}; {} goes on for {} more.",
                            Plural(Report.Common, "byte"), Longer, Plural(More, "byte")));
    } else {
        SayLine(std::format("These are bytes, not lines. They first differ at byte 0x{:x} ({}).", Report.FirstAt, Report.FirstAt));
        SayLine(std::format("{} of the first {} {}.", Report.Differing, Plural(Report.Common, "byte"),
                            Report.Differing == 1 ? "differs" : "differ"));
        if (FirstBytes.size() != SecondBytes.size()) {
            std::string_view Longer = FirstBytes.size() > SecondBytes.size() ? FirstLabel : SecondLabel;
            std::size_t      More   = std::max(FirstBytes.size(), SecondBytes.size()) - Report.Common;
            SayLine(std::format("{} goes on for {} more than the other.", Longer, Plural(More, "byte")));
        }
    }

    bool        WithText = GlassWidth() == 0 || GlassWidth() >= HexRowWithText;
    std::size_t Base     = Report.FirstAt / BytesARow * BytesARow;
    SayLine("");
    HexRow("-", COLOR_RED, COLOR_DARK_RED, FirstBytes, SecondBytes, Base, WithText);
    HexRow("+", COLOR_GREEN, COLOR_DARK_GREEN, SecondBytes, FirstBytes, Base, WithText);
}
