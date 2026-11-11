/* record.cpp — the plants' file: its text, read and written. */

#include "record.h"

#include <algorithm>
#include <charconv>

#include "keep.h"

namespace garden {

namespace {

constexpr Keep             PlantsFile{"garden.plants", "garden,plants"};
constexpr std::string_view Heading = "garden 1";
constexpr std::size_t      Fields  = 5;

// A name that holds nothing a line could not.
std::string Plain(std::string_view Name)
{
    std::string Kept(Name);
    std::replace_if(Kept.begin(), Kept.end(), [](char C) { return C == '\t' || C == '\n' || C == '\r'; }, '_');
    return Kept;
}

bool Number(std::string_view Field, std::uint64_t &Into)
{
    const std::from_chars_result Parsed = std::from_chars(Field.data(), Field.data() + Field.size(), Into);
    return Parsed.ec == std::errc() && Parsed.ptr == Field.data() + Field.size();
}

// The plant a line says: name, starts, planted, last and system, a tab between.
std::optional<Plant> PlantOf(std::string_view Line)
{
    std::string_view Said[Fields];
    for (std::size_t At = 0; At < Fields; At++) {
        const std::size_t Tab  = Line.find('\t');
        const bool        Last = At + 1 == Fields;
        if (Last == (Tab != std::string_view::npos)) return std::nullopt;
        Said[At] = Line.substr(0, Tab);
        if (!Last) Line.remove_prefix(Tab + 1);
    }
    std::uint64_t Starts = 0, Planted = 0, Last = 0, System = 0;
    if (Said[0].empty() || !Number(Said[1], Starts) || !Number(Said[2], Planted) || !Number(Said[3], Last) ||
        !Number(Said[4], System) || Starts == 0 || Starts > UINT32_MAX || System > 1)
        return std::nullopt;
    return Plant{std::string(Said[0]), static_cast<std::uint32_t>(Starts), Planted, Last, System == 1};
}

}  // namespace

void Record::Start(std::string_view Name, bool System, std::uint64_t Now)
{
    const std::string Kept  = Plain(Name);
    const auto        Found = std::find_if(Plants.begin(), Plants.end(), [&](const Plant &One) { return One.Name == Kept; });
    if (Found == Plants.end()) {
        Plants.push_back(Plant{Kept, 1, Now, Now, System});
        return;
    }
    if (Found->Starts < UINT32_MAX) Found->Starts++;
    Found->Last   = std::max(Found->Last, Now);
    Found->System = System;
}

std::string Record::Text() const
{
    std::string Text(Heading);
    Text += '\n';
    for (const Plant &One : Plants) {
        Text += One.Name;
        for (const std::uint64_t Field : {static_cast<std::uint64_t>(One.Starts), One.Planted, One.Last,
                                          static_cast<std::uint64_t>(One.System)}) {
            Text += '\t';
            Text += std::to_string(Field);
        }
        Text += '\n';
    }
    return Text;
}

Record Record::FromText(std::string_view Text)
{
    Record Read;
    bool   AtHeading = true;
    while (!Text.empty()) {
        const std::size_t End = Text.find('\n');
        if (End == std::string_view::npos) break;
        const std::string_view Line = Text.substr(0, End);
        Text.remove_prefix(End + 1);
        if (AtHeading) {
            AtHeading = false;
            if (Line != Heading) break;
            continue;
        }
        if (std::optional<Plant> One = PlantOf(Line)) Read.Plants.push_back(std::move(*One));
    }
    return Read;
}

std::optional<std::string> ReadRecord(Record &Into)
{
    std::string Text;
    if (std::optional<std::string> Trouble = KeepRead(PlantsFile, Text)) return Trouble;
    Into = Record::FromText(Text);
    return std::nullopt;
}

std::optional<std::string> WriteRecord(const Record &From)
{
    return KeepWrite(PlantsFile, From.Text());
}

TouchTagPair GrewTag()
{
    return touch_intern("garden:grew");
}

}  // namespace garden
