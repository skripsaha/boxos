/* typer_record.cpp — the line the record file is made of, one run to a line and
 * readable by a person:
 *
 *   2026-11-02 14:03 | 62 wpm | 97% | 60 s | prose-60
 *
 * A line that does not parse is passed over, not mended: the file is the
 * person's too, and may have been edited by hand. */

#include "typer.h"

namespace {

constexpr std::size_t FieldCount = 5;
constexpr unsigned    NumberMost = 99999;

std::string_view Trim(std::string_view Text)
{
    while (!Text.empty() && (Text.front() == ' ' || Text.front() == '\t' || Text.front() == '\r' || Text.front() == '\n')) Text.remove_prefix(1);
    while (!Text.empty() && (Text.back() == ' ' || Text.back() == '\t' || Text.back() == '\r' || Text.back() == '\n')) Text.remove_suffix(1);
    return Text;
}

/* The number that leads Field, and the suffix it is followed by. */
bool Number(std::string_view Field, std::string_view Suffix, unsigned &Out)
{
    std::size_t Digits = 0;
    unsigned    Value  = 0;
    while (Digits < Field.size() && Field[Digits] >= '0' && Field[Digits] <= '9') {
        Value = Value * 10 + static_cast<unsigned>(Field[Digits] - '0');
        if (++Digits > 5) return false;
    }
    if (Digits == 0 || Value > NumberMost) return false;
    if (Trim(Field.substr(Digits)) != Suffix) return false;
    Out = Value;
    return true;
}

bool PlainWord(std::string_view Word)
{
    if (Word.empty()) return false;
    for (char Ch : Word)
        if (Ch <= ' ' || Ch >= 0x7F || Ch == '|') return false;
    return true;
}

}

std::string FormatRecord(const RecordLine &Line)
{
    return Line.When + " | " + std::to_string(Line.Wpm) + " wpm | " + std::to_string(Line.Accuracy) + "% | " +
           std::to_string(Line.Seconds) + " s | " + Line.Kind + "\n";
}

std::optional<RecordLine> ParseRecord(std::string_view Text)
{
    std::string_view Fields[FieldCount];
    std::size_t      Count = 0;
    while (true) {
        std::size_t Bar = Text.find('|');
        if (Count == FieldCount) return std::nullopt;
        Fields[Count++] = Trim(Text.substr(0, Bar));
        if (Bar == std::string_view::npos) break;
        Text.remove_prefix(Bar + 1);
    }
    if (Count != FieldCount) return std::nullopt;

    RecordLine Line;
    if (Fields[0].empty() || Fields[0].size() > 32) return std::nullopt;
    if (!Number(Fields[1], "wpm", Line.Wpm) || !Number(Fields[2], "%", Line.Accuracy) ||
        !Number(Fields[3], "s", Line.Seconds) || !PlainWord(Fields[4]))
        return std::nullopt;
    Line.When = std::string(Fields[0]);
    Line.Kind = std::string(Fields[4]);
    return Line;
}

std::optional<unsigned> BestWpm(std::string_view Text, std::string_view Kind)
{
    std::optional<unsigned> Best;
    while (!Text.empty()) {
        std::size_t      End  = Text.find('\n');
        std::string_view Line = Text.substr(0, End);
        Text = End == std::string_view::npos ? std::string_view() : Text.substr(End + 1);

        std::optional<RecordLine> Run = ParseRecord(Line);
        if (Run && Run->Kind == Kind && (!Best || Run->Wpm > *Best)) Best = Run->Wpm;
    }
    return Best;
}
