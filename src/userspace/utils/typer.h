#ifndef TYPER_H
#define TYPER_H

/* typer — a typing trainer and speed test that owns the whole screen.
 *
 * The parts: typer_passages.cpp is the shelf of texts, typer_text.cpp turns the
 * Form's words and a file's bytes into a Task and a passage and wraps it,
 * typer_attempt.cpp judges keystrokes, typer_record.cpp is the line the record
 * file is made of, typer_store.cpp keeps that file on the volume,
 * typer_screen.cpp is the glass and typer_paint.cpp / typer_results.cpp draw on
 * it; typer.cpp decides.
 *
 * This header knows nothing of BoxOS — what it names is plain C++ and can be
 * built and tried anywhere; typer_screen.h is the screen, the keys and the volume.
 */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class TextKind { Prose, Code, File };

/* What the person asked for: the kind of text, and a time limit in seconds
 * (0 for none, the test ends with the text). */
struct Task {
    TextKind Kind    = TextKind::Prose;
    unsigned Seconds = 0;
};

constexpr unsigned    SecondsLeast = 5;
constexpr unsigned    SecondsMost  = 3600;
constexpr std::size_t FileCharsMost = 3000;

/* The Form's words and whether a file came with them. A refusal is a
 * sentence; none means Out is filled. */
std::optional<std::string> ReadTask(const std::vector<std::string> &Words, bool HasFile, Task &Out);

/* The kind spelled for the record: prose, prose-60, code, code-60, file. */
std::string KindLabel(const Task &Job);

struct Prepared {
    std::string Text;
    std::size_t Skipped = 0;     /* characters the keyboard cannot type */
    bool        Cut     = false; /* the file had more than FileCharsMost */
};

/* A file's bytes made into text a keyboard can type. A refusal is a sentence. */
std::optional<std::string> PrepareFileText(const std::uint8_t *Bytes, std::size_t Count, Prepared &Out);

struct Line {
    std::size_t Start  = 0;
    std::size_t Length = 0;
};

/* The text cut into lines of at most Width characters, a word never split
 * unless it is longer than a line. A space stays at the end of the line it
 * follows, so every character of Text is on exactly one line. */
std::vector<Line> Wrap(std::string_view Text, std::size_t Width);

struct Dice {
    std::uint64_t State;

    explicit Dice(std::uint64_t Seed) : State(Seed) {}
    std::uint64_t Roll();
    std::size_t   Below(std::size_t Count);
};

/* Passages of one kind in random order, never the same one twice running. */
class Shelf {
public:
    Shelf(TextKind Kind, std::uint64_t Seed);
    std::string_view Next();

private:
    TextKind    Kind;
    Dice        Rolls;
    std::size_t Last;
};

std::size_t      PassageCount(TextKind Kind);
std::string_view PassageAt(TextKind Kind, std::size_t Index);

enum class Mark : std::uint8_t { Untyped, Right, Wrong };

struct Missed {
    char     Key   = 0;
    unsigned Count = 0;
};

/* One run at a text. Time is handed in, never read, so the judging is the
 * same on any clock: the clock starts at the first key. */
class Attempt {
public:
    Attempt(std::string Text, unsigned Seconds);

    /* False when the key counted for nothing: the time is up or the text is
     * all typed. */
    bool Type(char Key, std::uint64_t NowMs);
    void Back();
    void Append(std::string_view More);
    void Stop(std::uint64_t NowMs);

    bool          Started() const { return Began; }
    bool          Finished(std::uint64_t NowMs) const;
    bool          Timed() const { return Seconds != 0; }
    std::uint64_t ElapsedMs(std::uint64_t NowMs) const;
    std::uint64_t LimitMs() const { return static_cast<std::uint64_t>(Seconds) * 1000; }

    /* Characters standing right, as words of five, over the time taken. */
    unsigned Wpm(std::uint64_t NowMs) const;
    /* Keystrokes that were right, whether or not taken back later. */
    unsigned Accuracy() const;
    unsigned Keys() const { return KeyCount; }
    unsigned Mistakes() const { return WrongKeys; }
    unsigned Corrections() const { return BackCount; }

    const std::string &Text() const { return Chars; }
    std::size_t        Cursor() const { return Place; }
    Mark               MarkAt(std::size_t Index) const { return Marks[Index]; }

    std::vector<Missed> MostMissed(std::size_t Count) const;
    /* Speed in each slice of the run, in words per minute. */
    std::vector<unsigned> SpeedBars(unsigned SliceSeconds, std::uint64_t ElapsedMs) const;

private:
    struct Stroke {
        std::uint32_t Ms;
        bool          Right;
    };

    std::string       Chars;
    std::vector<Mark> Marks;
    unsigned          Seconds;
    std::size_t       Place = 0;
    bool              Began = false;
    bool              Ended = false;
    std::uint64_t     StartMs = 0;
    std::uint64_t     EndMs   = 0;
    unsigned          RightStanding = 0;
    unsigned          KeyCount  = 0;
    unsigned          WrongKeys = 0;
    unsigned          BackCount = 0;
    unsigned          MissCounts[256] = {};
    std::vector<Stroke> Strokes;
};

/* The slice, in seconds, that shows a run of ElapsedMs in at most MaxBars bars:
 * five seconds, or more when the run is long. */
unsigned SliceSecondsFor(std::uint64_t ElapsedMs, unsigned MaxBars);

/* "12.4" for 12400 milliseconds. */
std::string SecondsText(std::uint64_t Ms);

struct RecordLine {
    std::string When;
    unsigned    Wpm      = 0;
    unsigned    Accuracy = 0;
    unsigned    Seconds  = 0;
    std::string Kind;
};

/* "2026-11-02 14:03 | 62 wpm | 97% | 60 s | prose-60", newline included. */
std::string                FormatRecord(const RecordLine &Line);
std::optional<RecordLine>  ParseRecord(std::string_view Line);
/* The best speed among the lines of Kind in a record file's text; damaged
 * lines are passed over. */
std::optional<unsigned>    BestWpm(std::string_view RecordText, std::string_view Kind);

#endif /* TYPER_H */
