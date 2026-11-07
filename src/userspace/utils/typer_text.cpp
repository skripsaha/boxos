/* typer_text.cpp — from what was asked and what was read to a text that can be
 * typed and laid out: the Task, a file made typeable, the wrapped lines and the
 * dice that choose a passage. None of it touches the machine. */

#include "typer.h"

namespace {

constexpr std::size_t BinaryTenth = 10;

bool OnlyDigits(const std::string &Word)
{
    if (Word.empty()) return false;
    for (char Ch : Word)
        if (Ch < '0' || Ch > '9') return false;
    return true;
}

bool IsSpaceCode(std::uint32_t Code)
{
    return Code == '\t' || Code == '\n' || Code == '\r' || Code == '\f' || Code == '\v' || Code == 0xA0 ||
           (Code >= 0x2000 && Code <= 0x200A) || Code == 0x3000;
}

/* The code point led by Bytes[At], and how many bytes it took; 0 for a byte
 * that leads no well-formed sequence. */
std::size_t Decode(const std::uint8_t *Bytes, std::size_t Count, std::size_t At, std::uint32_t &Code)
{
    std::uint8_t Lead = Bytes[At];
    if (Lead < 0x80) {
        Code = Lead;
        return 1;
    }
    std::size_t   Need;
    std::uint32_t Low;
    if (Lead >= 0xC2 && Lead <= 0xDF)      { Need = 1; Code = Lead & 0x1Fu; Low = 0x80; }
    else if (Lead >= 0xE0 && Lead <= 0xEF) { Need = 2; Code = Lead & 0x0Fu; Low = 0x800; }
    else if (Lead >= 0xF0 && Lead <= 0xF4) { Need = 3; Code = Lead & 0x07u; Low = 0x10000; }
    else return 0;
    if (At + Need >= Count) return 0;
    for (std::size_t I = 1; I <= Need; I++) {
        if ((Bytes[At + I] & 0xC0) != 0x80) return 0;
        Code = (Code << 6) | (Bytes[At + I] & 0x3Fu);
    }
    return Code < Low ? 0 : Need + 1;
}

bool LooksBinary(const std::uint8_t *Bytes, std::size_t Count)
{
    std::size_t Controls = 0;
    for (std::size_t I = 0; I < Count; I++) {
        std::uint8_t Byte = Bytes[I];
        if (Byte == 0) return true;
        if (Byte < 0x20 && Byte != '\t' && Byte != '\n' && Byte != '\r' && Byte != '\f' && Byte != '\v') Controls++;
    }
    return Controls * BinaryTenth > Count;
}

/* What a code point is typed as: a space, one or more characters, or nothing. */
std::string_view Typed(std::uint32_t Code, char &One)
{
    if (IsSpaceCode(Code)) { One = ' '; return {}; }
    if (Code >= 0x20 && Code < 0x7F) { One = static_cast<char>(Code); return {}; }
    switch (Code) {
    case 0x2018: case 0x2019: One = '\''; return {};
    case 0x201C: case 0x201D: One = '"'; return {};
    case 0x2013: case 0x2014: case 0x2212: One = '-'; return {};
    case 0x2026: return "...";
    default: One = 0; return {};
    }
}

}

std::optional<std::string> ReadTask(const std::vector<std::string> &Words, bool HasFile, Task &Out)
{
    Out = Task{};
    if (HasFile) Out.Kind = TextKind::File;
    if (Words.empty()) return std::nullopt;
    if (Words.size() > 1) return std::string("typer takes one thing after its name: seconds for a timed test, or code");

    const std::string &Word = Words[0];
    if (Word == "code") {
        if (HasFile) return std::string("code and a file together make no sense: a file is typed as it is");
        Out.Kind = TextKind::Code;
        return std::nullopt;
    }
    if (!OnlyDigits(Word) || Word.size() > 5)
        return "'" + Word + "' is neither a number of seconds nor code: typer 60, typer code, typer note.txt";

    unsigned Seconds = static_cast<unsigned>(std::stoul(Word));
    if (Seconds < SecondsLeast || Seconds > SecondsMost)
        return "a timed test lasts from " + std::to_string(SecondsLeast) + " to " + std::to_string(SecondsMost) + " seconds";
    Out.Seconds = Seconds;
    return std::nullopt;
}

std::string KindLabel(const Task &Job)
{
    std::string Label = Job.Kind == TextKind::Code ? "code" : Job.Kind == TextKind::File ? "file" : "prose";
    if (Job.Seconds != 0) Label += "-" + std::to_string(Job.Seconds);
    return Label;
}

std::optional<std::string> PrepareFileText(const std::uint8_t *Bytes, std::size_t Count, Prepared &Out)
{
    Out = Prepared{};
    if (Count == 0) return std::string("that file is empty: there is nothing to type");
    if (LooksBinary(Bytes, Count)) return std::string("that file is not text: typer types text");

    std::string &Text = Out.Text;
    for (std::size_t At = 0; At < Count && Text.size() <= FileCharsMost;) {
        std::uint32_t Code = 0;
        std::size_t   Took = Decode(Bytes, Count, At, Code);
        if (Took == 0) {
            Out.Skipped++;
            At++;
            continue;
        }
        At += Took;

        char             One = 0;
        std::string_view Many = Typed(Code, One);
        if (Many.empty() && One == 0) {
            Out.Skipped++;
            continue;
        }
        if (Many.empty()) Many = std::string_view(&One, 1);
        for (char Ch : Many) {
            if (Ch == ' ' && (Text.empty() || Text.back() == ' ')) continue;
            Text += Ch;
        }
    }

    if (Text.size() > FileCharsMost) {
        Out.Cut = true;
        bool MidWord = Text[FileCharsMost] != ' ';
        Text.resize(FileCharsMost);
        std::size_t Space = Text.rfind(' ');
        if (MidWord && Space != std::string::npos && Space > FileCharsMost / 2) Text.resize(Space);
    }
    while (!Text.empty() && Text.back() == ' ') Text.pop_back();
    if (Text.empty()) return std::string("that file has no text a keyboard can type");
    return std::nullopt;
}

std::vector<Line> Wrap(std::string_view Text, std::size_t Width)
{
    if (Width == 0) Width = 1;
    std::vector<Line> Lines;
    Line              Current;

    auto Close = [&] {
        if (Current.Length != 0) Lines.push_back(Current);
        Current = Line{};
    };

    std::size_t At = 0;
    while (At < Text.size()) {
        std::size_t End = At;
        while (End < Text.size() && Text[End] != ' ') End++;
        while (End < Text.size() && Text[End] == ' ') End++;
        std::size_t Length = End - At;

        if (Current.Length + Length > Width) Close();
        if (Current.Length == 0) Current.Start = At;
        while (Length > Width) {
            Lines.push_back(Line{At, Width});
            At += Width;
            Length -= Width;
            Current.Start = At;
        }
        Current.Length += Length;
        At += Length;
    }
    Close();
    return Lines;
}

std::uint64_t Dice::Roll()
{
    State += 0x9E3779B97F4A7C15ull;
    std::uint64_t Mixed = State;
    Mixed = (Mixed ^ (Mixed >> 30)) * 0xBF58476D1CE4E5B9ull;
    Mixed = (Mixed ^ (Mixed >> 27)) * 0x94D049BB133111EBull;
    return Mixed ^ (Mixed >> 31);
}

std::size_t Dice::Below(std::size_t Count)
{
    return Count == 0 ? 0 : static_cast<std::size_t>(Roll() % Count);
}

Shelf::Shelf(TextKind Kind, std::uint64_t Seed) : Kind(Kind), Rolls(Seed), Last(PassageCount(Kind)) {}

std::string_view Shelf::Next()
{
    std::size_t Count = PassageCount(Kind);
    std::size_t Pick  = Count > 1 ? Rolls.Below(Count - 1) : 0;
    if (Last < Count && Pick >= Last) Pick++;
    Last = Pick;
    return PassageAt(Kind, Pick);
}

std::string SecondsText(std::uint64_t Ms)
{
    return std::to_string(Ms / 1000) + "." + std::to_string(Ms % 1000 / 100);
}
