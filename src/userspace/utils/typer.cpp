/* typer — a typing trainer and speed test that owns the whole screen.
 *
 *   typer              a passage of everyday English, typed to its end
 *   typer 60           a timed test: passages keep coming until the time is up
 *   typer code         passages full of the symbols that are hard to reach
 *   typer NOTE         the text of a file
 *
 *   Backspace          takes a character back; the mistake still counts against accuracy
 *   Esc, ^Q            leave; a run left half done is said, not recorded
 *   Enter, q           on the results: another passage, leave
 *
 * The clock starts at the first key, not at the first frame, and the numbers in
 * the foot are redrawn five times a second even when no key comes. A finished
 * run is added as one line to typer.record, which is how the best is known.
 * Nothing may print between the first frame and the screen given back; the
 * parts are listed in typer.h.
 */

#include <format>
#include <new>

#include "box/clock.h"
#include "box/file.h"
#include "box/form.h"
#include "box/print.h"

#include "typer_screen.h"

BOX_FORM("takes optional thing \"a file whose text to type\"\n"
         "takes optional words \"seconds for a timed test, or code for symbols\"\n");

namespace {

constexpr std::uint32_t TickMs        = 200;
constexpr std::uint32_t FlourishMs    = 120;
constexpr std::size_t   FeedAhead     = 300;
constexpr std::uint64_t FileBytesMost = 65536;

enum class Verdict { Finished, Left };

struct Summary {
    bool          Typed = false;
    bool          Kept  = false;
    unsigned      Wpm   = 0;
    unsigned      Accuracy = 100;
    std::uint64_t Ms    = 0;
};

void Say(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_to_screen(Line.c_str());
}

/* The file's text made typeable, and what was done to it said in Note. */
std::optional<std::string> LoadFile(file_id_t File, Prepared &Text, std::string &Note)
{
    FileMetadata Metadata;
    if (FileMetadataRead(File, &Metadata) != 0) return std::string("typer: could not look at that file; it may have been erased");
    std::uint64_t Size = std::min<std::uint64_t>(Metadata.Size, FileBytesMost);
    FileMetadataRelease(&Metadata);

    std::vector<std::uint8_t> Bytes(static_cast<std::size_t>(Size));
    std::size_t               Have = 0;
    while (Have < Bytes.size()) {
        std::int64_t Got = fread(File, Have, Bytes.data() + Have, Bytes.size() - Have);
        if (Got < 0) return std::string("typer: the file would not be read");
        if (Got == 0) break;
        Have += static_cast<std::size_t>(Got);
    }

    if (std::optional<std::string> Refusal = PrepareFileText(Bytes.data(), Have, Text)) return "typer: " + *Refusal;
    if (Text.Skipped != 0)
        Note = std::format("{} character{} the keyboard cannot type {} skipped", Text.Skipped, Text.Skipped == 1 ? "" : "s",
                           Text.Skipped == 1 ? "was" : "were");
    if (Text.Cut) {
        if (!Note.empty()) Note += "; ";
        Note += std::format("the file is long, so only its first {} characters are typed", Text.Text.size());
    }
    return std::nullopt;
}

std::string Stamp()
{
    BoxTime Now;
    if (clock_boxtime_local(&Now) != 0 && clock_boxtime(&Now) != 0) return "unknown time";
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}", unsigned{Now.year}, unsigned{Now.month}, unsigned{Now.day},
                       unsigned{Now.hour}, unsigned{Now.minute});
}

std::uint64_t Seed()
{
    return clock_unix_now_ns() ^ (clock_uptime_us() * 0x9E3779B97F4A7C15ull);
}

class Trainer {
public:
    Trainer(const Task &Job, Prepared FileText, std::string Note)
        : Job(Job), FileText(std::move(FileText)), Note(std::move(Note)), Books(Job.Kind, Seed())
    {
    }

    std::optional<std::string> Open() { return Display.Take(); }
    void                       Play();
    void                       Close() { Display.Give(); }
    std::string                Said() const;

private:
    Attempt NewAttempt();
    Verdict Type(Attempt &Run);
    bool    Handle(Attempt &Run, const painter::tui::KeyEvent &Key, std::vector<Line> &Lines);
    void    Remember(const Attempt &Run, std::uint64_t Ms, bool Kept);
    bool    ShowResults(const Attempt &Run, std::uint64_t Ms);
    Report  Record(const Attempt &Run, std::uint64_t Ms);

    Task        Job;
    Prepared    FileText;
    std::string Note;
    Shelf       Books;
    Screen      Display;
    Summary     Last;
};

Attempt Trainer::NewAttempt()
{
    if (Job.Kind == TextKind::File) return Attempt(FileText.Text, Job.Seconds);
    return Attempt(std::string(Books.Next()), Job.Seconds);
}

/* True while the run goes on. */
bool Trainer::Handle(Attempt &Run, const painter::tui::KeyEvent &Key, std::vector<Line> &Lines)
{
    if (Key.IsCtrl()) return !(Key.Character == 'q' || Key.Character == 'Q');
    if (Key.IsExtended()) return true;

    char32_t Ch = Key.Character;
    if (Ch == painter::tui::KeyCodes::Escape) return false;
    if (Ch == painter::tui::KeyCodes::Backspace || Ch == 0x7f) {
        Run.Back();
    } else if (Ch >= 0x20 && Ch < 0x7f) {
        Run.Type(Ch, clock_uptime_ms());
        if (Job.Seconds != 0 && Job.Kind != TextKind::File && Run.Text().size() - Run.Cursor() < FeedAhead) {
            std::string More = " ";
            More += Books.Next();
            Run.Append(More);
            Lines = Wrap(Run.Text(), TextWidth(Display.Cols));
        }
    }
    return true;
}

Verdict Trainer::Type(Attempt &Run)
{
    std::vector<Line> Lines = Wrap(Run.Text(), TextWidth(Display.Cols));

    for (;;) {
        std::uint64_t Now = clock_uptime_ms();
        if (Run.Started() && Run.Finished(Now)) {
            Run.Stop(Now);
            return Verdict::Finished;
        }

        PaintTyping(Display, TypingView{Run, Job, Lines, Run.Started() ? std::string_view() : std::string_view(Note), Now});
        if (!Display.Flush()) return Verdict::Left;

        std::uint32_t Wait = 0;
        if (Run.Started()) {
            std::uint64_t Left = Run.Timed() ? Run.LimitMs() - Run.ElapsedMs(Now) : TickMs;
            Wait = static_cast<std::uint32_t>(std::max<std::uint64_t>(std::min<std::uint64_t>(Left, TickMs), 1));
        }

        painter::tui::KeyEvent Key;
        Heard      Got = Display.Listen(Key, Wait);
        if (Got == Heard::Gone) return Verdict::Left;
        if (Got == Heard::Key && !Handle(Run, Key, Lines)) return Verdict::Left;
    }
}

void Trainer::Remember(const Attempt &Run, std::uint64_t Ms, bool Kept)
{
    Last.Typed    = Run.Keys() != 0;
    Last.Kept     = Kept;
    Last.Wpm      = Run.Wpm(Ms);
    Last.Accuracy = Run.Accuracy();
    Last.Ms       = Ms;
}

/* Where the run stands against the record, and the run added to it. */
Report Trainer::Record(const Attempt &Run, std::uint64_t Ms)
{
    Report Result{Run, Job, Ms, Standing::NoRecord, 0, std::string()};
    std::string Book;
    if (std::optional<std::string> Trouble = ReadRecordText(Book)) {
        Result.Trouble = "The record was not read: " + *Trouble;
        return Result;
    }

    std::string                Label = KindLabel(Job);
    std::optional<unsigned>    Best  = BestWpm(Book, Label);
    unsigned                   Wpm   = Run.Wpm(Ms);
    if (!Best)           Result.Place = Standing::First;
    else if (Wpm > *Best) Result.Place = Standing::Beaten;
    else                  Result.Place = Standing::Short;
    Result.Best = Best.value_or(0);

    RecordLine Line{Stamp(), Wpm, Run.Accuracy(), static_cast<unsigned>((Ms + 500) / 1000), Label};
    if (std::optional<std::string> Trouble = AppendRecordLine(FormatRecord(Line)))
        Result.Trouble = "This run was not recorded: " + *Trouble;
    return Result;
}

/* True when the person wants another passage. */
bool Trainer::ShowResults(const Attempt &Run, std::uint64_t Ms)
{
    Report   Result = Record(Run, Ms);
    unsigned Beat   = 0;
    for (;;) {
        PaintResults(Display, Result, Beat++);
        if (!Display.Flush()) return false;

        painter::tui::KeyEvent Key;
        Heard      Got = Display.Listen(Key, Result.Place == Standing::Beaten ? FlourishMs : 0);
        if (Got == Heard::Gone) return false;
        if (Got != Heard::Key) continue;

        if (Key.IsCtrl()) {
            if (Key.Character == 'q' || Key.Character == 'Q') return false;
        } else if (!Key.IsExtended()) {
            if (Key.Character == painter::tui::KeyCodes::Enter) return true;
            if (Key.Character == painter::tui::KeyCodes::Escape || Key.Character == 'q' || Key.Character == 'Q') return false;
        }
    }
}

void Trainer::Play()
{
    for (;;) {
        Attempt Run = NewAttempt();
        Verdict How = Type(Run);
        if (How == Verdict::Left) {
            Remember(Run, Run.ElapsedMs(clock_uptime_ms()), false);
            return;
        }
        std::uint64_t Ms = Run.ElapsedMs(0);
        Remember(Run, Ms, true);
        if (!ShowResults(Run, Ms)) return;
    }
}

std::string Trainer::Said() const
{
    std::string Line;
    if (!Last.Typed) {
        Line = "[TYPER] left without typing";
    } else {
        Line = std::format("[TYPER] {} wpm, {}% accuracy over {} s", Last.Wpm, Last.Accuracy, (Last.Ms + 500) / 1000);
        if (!Last.Kept) Line += " (left before the end, not recorded)";
    }
    if (!Note.empty()) Line += "\n[TYPER] " + Note;
    return Line;
}

int Run()
{
    const file_id_t   *Files;
    std::size_t        FileCount;
    const char *const *Words;
    std::size_t        WordCount;
    if (form_things(0, &Files, &FileCount) != 0 || form_words(1, &Words, &WordCount) != 0) return 1;

    Task Job;
    if (std::optional<std::string> Refusal = ReadTask(std::vector<std::string>(Words, Words + WordCount), FileCount != 0, Job)) {
        Say("typer: " + *Refusal);
        return 1;
    }

    Prepared    FileText;
    std::string Note;
    if (FileCount != 0) {
        if (std::optional<std::string> Refusal = LoadFile(Files[0], FileText, Note)) {
            Say(*Refusal);
            return 1;
        }
    }

    Trainer Typing(Job, std::move(FileText), std::move(Note));
    if (std::optional<std::string> Refused = Typing.Open()) {
        Typing.Close();
        Say(*Refused);
        return 1;
    }
    Typing.Play();
    Typing.Close();
    /* The one line typer has to say goes where its printing goes: under an
     * arrow, `typer -> runs.txt` keeps a line a run in that file. */
    println(Typing.Said().c_str());
    return 0;
}

}

int main()
{
    try {
        return Run();
    } catch (const std::bad_alloc &) {
        Say("typer: there is no memory for this test");
        return 1;
    }
}
