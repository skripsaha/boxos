/* ledger.cpp — the file of what has been done (ledger.h). */

#include "ledger.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <optional>
#include <string_view>

#include "keep.h"

namespace hangman {

namespace {

const Keep LedgerFile{"hangman.ledger", "hangman,ledger"};

template <class Each>
void ForLines(std::string_view Text, Each Do)
{
    while (!Text.empty()) {
        const std::size_t End = Text.find('\n');
        const std::string_view Line = Text.substr(0, End);
        const std::size_t Gap = Line.find(' ');
        if (Gap != std::string_view::npos) Do(Line.substr(0, Gap), Line.substr(Gap + 1));
        Text = End == std::string_view::npos ? std::string_view() : Text.substr(End + 1);
    }
}

std::uint64_t Number(std::string_view Text)
{
    std::uint64_t Value = 0;
    std::from_chars(Text.data(), Text.data() + Text.size(), Value);
    return Value;
}

}  // namespace

Ledger::Ledger()
{
    std::string Text;
    if (const std::optional<std::string> Refusal = KeepRead(LedgerFile, Text)) {
        Said = *Refusal;
        Writable = false;
        return;
    }
    ForLines(Text, [&](std::string_view Word, std::string_view Rest) {
        const std::uint64_t Value = Number(Rest);
        if (Word == "games") Kept.Games = static_cast<std::uint32_t>(Value);
        else if (Word == "wins") Kept.Wins = static_cast<std::uint32_t>(Value);
        else if (Word == "streak") Kept.Streak = static_cast<std::uint32_t>(Value);
        else if (Word == "best") Kept.Best = static_cast<std::uint32_t>(Value);
        else if (Word == "score") Kept.Score = Value;
    });
}

void Ledger::Add(bool Won, std::uint32_t Points)
{
    Kept.Games++;
    if (Won) {
        Kept.Wins++;
        Kept.Streak++;
        Kept.Best = std::max(Kept.Best, Kept.Streak);
        Kept.Score += Points;
    } else {
        Kept.Streak = 0;
    }
    Save();
}

void Ledger::Save()
{
    if (!Writable) return;
    const std::string Text = std::format("games {}\nwins {}\nstreak {}\nbest {}\nscore {}\n", Kept.Games, Kept.Wins,
                                         Kept.Streak, Kept.Best, Kept.Score);
    if (const std::optional<std::string> Refusal = KeepWrite(LedgerFile, Text)) Said = *Refusal;
    else Said.clear();
}

}  // namespace hangman
