/* machine.cpp — the machine read: its memory, the roster and the registry of
 * tagged regions. */

#include "machine.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string_view>

#include "box/clock.h"
#include "box/file.h"
#include "box/memory.h"
#include "box/memtag.h"
#include "box/system.h"

namespace memmap {

namespace {

constexpr std::uint64_t PageBytes  = 4096;
constexpr std::size_t   TagsBuffer = 4096;

bool Holds(const std::vector<std::string_view> &Tags, std::string_view Tag)
{
    return std::find(Tags.begin(), Tags.end(), Tag) != Tags.end();
}

bool HoldsAnyOf(const std::vector<std::string_view> &Tags, std::initializer_list<std::string_view> These)
{
    return std::any_of(These.begin(), These.end(), [&](std::string_view One) { return Holds(Tags, One); });
}

bool BeginsWith(const std::vector<std::string_view> &Tags, std::string_view Prefix, std::string_view *Rest)
{
    for (std::string_view One : Tags)
        if (One.substr(0, Prefix.size()) == Prefix) {
            *Rest = One.substr(Prefix.size());
            return true;
        }
    return false;
}

std::uint32_t Number(std::string_view Digits)
{
    std::uint32_t Value = 0;
    for (const char Digit : Digits) {
        if (Digit < '0' || Digit > '9') return 0;
        Value = Value * 10 + static_cast<std::uint32_t>(Digit - '0');
    }
    return Value;
}

// What a region is, by its tags; false for one that says nothing of who holds
// memory (a mapping's cache type, a protection key).
bool Classify(const std::vector<std::string_view> &Tags, RegionSeen &Out)
{
    std::string_view Rest;
    if (BeginsWith(Tags, "cabin:", &Rest)) Out.Cabin = Number(Rest);
    if (Holds(Tags, "purpose:kernel")) Out.What = Kind::Image;
    else if (HoldsAnyOf(Tags, {"purpose:pocket-ring", "purpose:result-ring", "purpose:touch-ring"})) Out.What = Kind::Rings;
    else if (HoldsAnyOf(Tags, {"purpose:bay", "purpose:brook", "purpose:brook-header", "purpose:stream",
                               "purpose:shared"}))
        Out.What = Kind::Shared;
    else if (HoldsAnyOf(Tags, {"purpose:mmio", "purpose:firmware", "purpose:framebuffer", "purpose:dma",
                               "purpose:dma:encrypted"}))
        Out.What = Kind::Device;
    else if (BeginsWith(Tags, "zone:", &Rest)) Out.What = Kind::Untold;
    else return false;
    return true;
}

// The tag strings of one region, joined with commas; false when they could
// not be read.
bool ReadTags(std::uint32_t Id, std::string &Joined, std::vector<std::string> &Each)
{
    const std::unique_ptr<char[]> Buffer(new char[TagsBuffer]);
    std::uint32_t                 Count = 0;
    if (mem_region_tags(Id, Buffer.get(), TagsBuffer, &Count) != 0) return false;
    const char *At = Buffer.get();
    for (std::uint32_t Index = 0; Index < Count && At < Buffer.get() + TagsBuffer; Index++) {
        Each.emplace_back(At);
        At += Each.back().size() + 1;
    }
    for (const std::string &One : Each) Joined += (Joined.empty() ? "" : ",") + One;
    return true;
}

}  // namespace

std::string Machine::NameOf(file_id_t File, const std::string &FirstTag)
{
    if (File != 0) {
        if (const auto Known = Names.find(File); Known != Names.end()) return Known->second;
        FileMetadata Metadata;
        if (FileMetadataRead(File, &Metadata) == 0) {
            std::string Name = Metadata.Filename ? Metadata.Filename : "";
            FileMetadataRelease(&Metadata);
            if (const std::size_t Dot = Name.rfind('.'); Dot != std::string::npos && Dot > 0) Name.resize(Dot);
            if (!Name.empty()) return Names.emplace(File, std::move(Name)).first->second;
        }
    }
    return FirstTag;
}

// The regions again when the registry has changed since they were read; the
// tags of one that stands as it did are not asked for twice.
bool Machine::ReadRegions()
{
    mem_stats_t Stats{};
    if (mem_stats(&Stats) != 0) return false;
    if (Scanned && Stats.region_generation == RegionGeneration && Stats.bitmap_generation == BitmapGeneration)
        return true;
    std::vector<RegionSeen> Found;
    for (std::uint32_t Id = 0; Id < Stats.region_slot_count; Id++) {
        mem_region_info_t Info{};
        if (mem_region_info(Id, &Info) != 0) {
            Seen.erase(Id);
            continue;
        }
        Cached &Slot = Seen[Id];
        if (Slot.Generation != Info.generation || Slot.Region.Base != Info.base_phys ||
            Slot.Region.Bytes != Info.pages * PageBytes) {
            Slot = Cached{.Generation = Info.generation,
                          .Region     = RegionSeen{.Base  = Info.base_phys,
                                                  .Bytes = Info.pages * PageBytes,
                                                  .What  = Kind::Untold,
                                                  .Cabin = 0,
                                                  .Tags  = {}},
                          .Kept       = false};
            std::string              Joined;
            std::vector<std::string> Each;
            if (ReadTags(Id, Joined, Each)) {
                const std::vector<std::string_view> Views(Each.begin(), Each.end());
                Slot.Kept        = Classify(Views, Slot.Region);
                Slot.Region.Tags = std::move(Joined);
            }
        }
        if (Slot.Kept) Found.push_back(Slot.Region);
    }
    Regions          = std::move(Found);
    RegionGeneration = Stats.region_generation;
    BitmapGeneration = Stats.bitmap_generation;
    Scanned          = true;
    return true;
}

std::expected<Reading, int> Machine::Read()
{
    Reading Now;
    Now.AtUs = clock_uptime_us();

    RosterProgram *Raw   = nullptr;
    const int      Count = proc_roster(&Raw, nullptr);
    if (Count < 0) return std::unexpected(Count);
    const std::unique_ptr<RosterProgram, void (*)(void *)> Held(Raw, free);
    Now.Programs.reserve(static_cast<std::size_t>(Count));
    for (int Index = 0; Index < Count; Index++) {
        const RosterProgram &One  = Raw[Index];
        const std::string    Tags = One.tags ? One.tags : "";
        Now.Programs.push_back(ProgramSeen{.Pid        = One.pid,
                                           .Generation = One.generation,
                                           .File       = One.program_file,
                                           .BeganUs    = One.began_us,
                                           .Bytes      = One.memory_bytes,
                                           .Name       = NameOf(One.program_file, Tags.substr(0, Tags.find(','))),
                                           .Tags       = Tags});
    }

    system_info_t Info{};
    if (sysinfo(&Info) == 0) {
        Now.Total = Info.total_memory;
        Now.Used  = Info.used_memory;
    }
    Now.RegionsKnown = ReadRegions();
    Now.Regions      = Regions;
    for (const RegionSeen &One : Now.Regions)
        if (One.What == Kind::Untold) Now.Span = std::max(Now.Span, One.Base + One.Bytes);
    return Now;
}

}  // namespace memmap
