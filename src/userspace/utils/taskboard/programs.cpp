/* programs.cpp — the programs kept from one reading to the next. */

#include "programs.h"

#include <algorithm>
#include <cctype>

#include "box/file.h"
#include "box/life.h"

namespace taskboard {

namespace {

// The program of `Seen` it was started from, in the same reading: the one a
// strand of which started it.
const ProgramSeen *SpawnerOf(const Reading &Now, const ProgramSeen &Seen)
{
    if (Seen.SpawnerPid == 0) return nullptr;
    for (const ProgramSeen &One : Now.Programs)
        for (const StrandSeen &Strand : One.Strands)
            if (Strand.Pid == Seen.SpawnerPid && Strand.Generation == Seen.SpawnerGeneration) return &One;
    return nullptr;
}

// Let go to live behind, or started from one that was, however far up: a
// program started from one behind is behind with it (box/display.h).
bool HeldBehind(const Reading &Now, const ProgramSeen &Seen)
{
    const ProgramSeen *At = &Seen;
    for (std::size_t Steps = 0; At && Steps <= Now.Programs.size(); Steps++) {
        if (At->Places & PlaceBehind) return true;
        At = SpawnerOf(Now, *At);
    }
    return false;
}

Living WhereOf(const Reading &Now, const ProgramSeen &Seen, std::uint8_t Life)
{
    if (Seen.Places & PlaceShown) return Living::OnScreen;
    if (HeldBehind(Now, Seen)) return Living::Behind;
    if (Seen.Places & PlaceKeeper) return Living::Keeper;
    if (Life == PROGRAM_STAYS) return Living::OnItsOwn;
    if (Life == PROGRAM_SERVES) return Living::Serving;
    return Living::Task;
}

// The share of one core spent between two counts of processor time.
double Share(std::uint64_t Before, std::uint64_t After, std::uint64_t ElapsedUs)
{
    if (ElapsedUs == 0 || After < Before) return 0;
    return static_cast<double>(After - Before) / static_cast<double>(ElapsedUs);
}

bool HoldsFolded(std::string_view Text, std::string_view Part)
{
    if (Part.empty()) return true;
    const auto Fold = [](char C) { return static_cast<char>(std::tolower(static_cast<unsigned char>(C))); };
    return std::search(Text.begin(), Text.end(), Part.begin(), Part.end(),
                       [&](char A, char B) { return Fold(A) == Fold(B); }) != Text.end();
}

}  // namespace

const char *LivingName(Living Where) noexcept
{
    switch (Where) {
    case Living::OnScreen: return "on screen";
    case Living::Behind:   return "behind";
    case Living::Keeper:   return "keeper";
    case Living::OnItsOwn: return "on its own";
    case Living::Serving:  return "serving";
    case Living::Task:     break;
    }
    return "task";
}

bool Wears(std::string_view Tags, std::string_view Tag) noexcept
{
    while (!Tags.empty()) {
        const std::size_t Comma = Tags.find(',');
        std::string_view  One   = Tags.substr(0, Comma);
        while (!One.empty() && One.front() == ' ') One.remove_prefix(1);
        while (!One.empty() && One.back() == ' ') One.remove_suffix(1);
        if (One == Tag) return true;
        if (Comma == std::string_view::npos) break;
        Tags.remove_prefix(Comma + 1);
    }
    return false;
}

std::string Programs::NameOf(const ProgramSeen &Seen)
{
    if (Seen.File != 0) {
        if (const auto Known = Names.find(Seen.File); Known != Names.end()) return Known->second;
        FileMetadata Metadata;
        if (FileMetadataRead(Seen.File, &Metadata) == 0) {
            std::string Name = Metadata.Filename ? Metadata.Filename : "";
            FileMetadataRelease(&Metadata);
            if (const std::size_t Dot = Name.rfind('.'); Dot != std::string::npos && Dot > 0) Name.resize(Dot);
            if (!Name.empty()) return Names.emplace(Seen.File, std::move(Name)).first->second;
        }
    }
    // No file to name it by: its first tag, which a program's file gives it.
    return Seen.Tags.substr(0, Seen.Tags.find(','));
}

std::uint8_t Programs::LifeOf(file_id_t File)
{
    if (File == 0) return PROGRAM_TASK;
    if (const auto Known = Lives.find(File); Known != Lives.end()) return Known->second;
    ProgramLife Life{};
    const std::uint8_t Living = program_life(File, &Life) == 0 ? Life.Living : static_cast<std::uint8_t>(PROGRAM_TASK);
    Lives.emplace(File, Living);
    return Living;
}

void Programs::Take(const Reading &Now)
{
    const std::uint64_t  Elapsed = LastAtUs != 0 && Now.AtUs > LastAtUs ? Now.AtUs - LastAtUs : 0;
    std::vector<Program> Fresh;
    Fresh.reserve(Now.Programs.size());
    for (const ProgramSeen &Seen : Now.Programs) {
        Program One{.Seen = Seen, .Name = NameOf(Seen), .Where = WhereOf(Now, Seen, LifeOf(Seen.File)),
                    .System = Wears(Seen.Tags, "system"), .Processor = 0, .StrandProcessor = {}};
        const Program *Before = Find(Seen.Pid, Seen.Generation);
        if (Before) One.Processor = Share(Before->Seen.CpuUs(), Seen.CpuUs(), Elapsed);
        One.StrandProcessor.reserve(Seen.Strands.size());
        for (const StrandSeen &Strand : Seen.Strands) {
            double Used = 0;
            if (Before)
                for (const StrandSeen &Was : Before->Seen.Strands)
                    if (Was.Pid == Strand.Pid && Was.Generation == Strand.Generation)
                        Used = Share(Was.CpuUs, Strand.CpuUs, Elapsed);
            One.StrandProcessor.push_back(Used);
        }
        Fresh.push_back(std::move(One));
    }
    Gone.clear();
    for (Program &Was : List)
        if (std::none_of(Fresh.begin(), Fresh.end(), [&](const Program &Is) {
                return Is.Seen.Pid == Was.Seen.Pid && Is.Seen.Generation == Was.Seen.Generation;
            }))
            Gone.push_back(std::move(Was));
    List     = std::move(Fresh);
    LastAtUs = Now.AtUs;
}

const Program *Programs::Find(std::uint32_t Pid, std::uint32_t Generation) const noexcept
{
    for (const Program &One : List)
        if (One.Seen.Pid == Pid && One.Seen.Generation == Generation) return &One;
    return nullptr;
}

const Program *Programs::OwnerOf(std::uint32_t Pid, std::uint32_t Generation) const noexcept
{
    for (const Program &One : List)
        for (const StrandSeen &Strand : One.Seen.Strands)
            if (Strand.Pid == Pid && Strand.Generation == Generation) return &One;
    return nullptr;
}

const Program *Programs::Recall(std::uint32_t Pid, std::uint32_t Generation) const noexcept
{
    if (const Program *Here = Find(Pid, Generation)) return Here;
    for (const Program &One : Gone)
        if (One.Seen.Pid == Pid && One.Seen.Generation == Generation) return &One;
    return nullptr;
}

std::vector<const Program *> Programs::Shown(const Look &Settings, std::string_view Filter) const
{
    std::vector<const Program *> Kept;
    for (const Program &One : List)
        if (HoldsFolded(One.Name, Filter) || HoldsFolded(One.Seen.Tags, Filter)) Kept.push_back(&One);
    std::stable_sort(Kept.begin(), Kept.end(), [&](const Program *A, const Program *B) {
        if (Settings.Group == GroupBy::Living && A->Where != B->Where) return A->Where < B->Where;
        switch (Settings.Sort) {
        case SortBy::Memory:
            if (A->Seen.MemoryBytes != B->Seen.MemoryBytes) return A->Seen.MemoryBytes > B->Seen.MemoryBytes;
            break;
        case SortBy::Name:
            if (A->Name != B->Name) return A->Name < B->Name;
            break;
        case SortBy::Age:
            if (A->Seen.BeganUs != B->Seen.BeganUs) return A->Seen.BeganUs < B->Seen.BeganUs;
            break;
        case SortBy::Processor:
            if (A->Processor != B->Processor) return A->Processor > B->Processor;
            break;
        }
        return A->Seen.Pid < B->Seen.Pid;
    });
    return Kept;
}

}  // namespace taskboard
