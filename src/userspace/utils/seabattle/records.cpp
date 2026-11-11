/* records.cpp — the hall of records and its file (records.h). */

#include "records.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <optional>

#include "keep.h"

namespace seabattle {

namespace {

const Keep RecordFile{"seabattle.records", "seabattle,records"};

constexpr std::size_t MostKept = 40;

char Lower(char Letter)
{
    return Letter >= 'A' && Letter <= 'Z' ? static_cast<char>(Letter - 'A' + 'a') : Letter;
}

bool SameName(std::string_view One, std::string_view Other)
{
    return std::ranges::equal(One, Other, [](char A, char B) { return Lower(A) == Lower(B); });
}

// A line "games wins shots hits name": the record, or none when it is not one.
std::optional<Record> ReadLine(std::string_view Line)
{
    Record      Got;
    int *const  Numbers[] = {&Got.Games, &Got.Wins, &Got.Shots, &Got.Hits};
    const char *At = Line.data(), *const End = Line.data() + Line.size();
    for (int *Number : Numbers) {
        const auto Parsed = std::from_chars(At, End, *Number);
        if (Parsed.ec != std::errc() || Parsed.ptr == End || *Parsed.ptr != ' ') return std::nullopt;
        At = Parsed.ptr + 1;
    }
    Got.Name.assign(At, static_cast<std::size_t>(End - At));
    if (Got.Name.empty()) return std::nullopt;
    return Got;
}

}  // namespace

Records::Records()
{
    std::string Text;
    if (const std::optional<std::string> Refusal = KeepRead(RecordFile, Text)) {
        Said = *Refusal;
        Writable = false;
        return;
    }
    std::size_t From = 0;
    while (From < Text.size()) {
        std::size_t To = Text.find('\n', From);
        if (To == std::string::npos) To = Text.size();
        if (const std::optional<Record> Got = ReadLine(std::string_view(Text).substr(From, To - From))) Kept.push_back(*Got);
        From = To + 1;
    }
    Rank();
}

void Records::Rank()
{
    std::ranges::stable_sort(Kept, [](const Record &One, const Record &Other) {
        if (One.Wins != Other.Wins) return One.Wins > Other.Wins;
        if (One.Games != Other.Games) return One.Games < Other.Games;
        return One.AccuracyPercent() > Other.AccuracyPercent();
    });
    if (Kept.size() > MostKept) Kept.resize(MostKept);
}

void Records::Add(std::string_view Name, bool Won, int Shots, int Hits)
{
    auto Found = std::ranges::find_if(Kept, [&](const Record &One) { return SameName(One.Name, Name); });
    if (Found == Kept.end()) {
        Kept.push_back(Record{.Name = std::string(Name)});
        Found = Kept.end() - 1;
    }
    Found->Games++;
    if (Won) Found->Wins++;
    Found->Shots += Shots;
    Found->Hits += Hits;
    Rank();
    Save();
}

void Records::Save()
{
    if (!Writable) return;
    std::string Text;
    for (const Record &One : Kept) Text += std::format("{} {} {} {} {}\n", One.Games, One.Wins, One.Shots, One.Hits, One.Name);
    if (const std::optional<std::string> Refusal = KeepWrite(RecordFile, Text)) Said = *Refusal;
    else Said.clear();
}

}  // namespace seabattle
