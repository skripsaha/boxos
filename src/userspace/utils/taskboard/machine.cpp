/* machine.cpp — the machine read: the roster, the display's places, the cores,
 * memory, the volume and the Use Context. */

#include "machine.h"

#include <algorithm>
#include <cstring>
#include <memory>

#include "box/clock.h"
#include "box/display.h"
#include "box/error.h"
#include "box/memory.h"
#include "box/system.h"
#include "box/use.h"

namespace taskboard {

namespace {

// Every program aboard, as the kernel's roster has it.
std::expected<std::vector<ProgramSeen>, int> ReadRoster()
{
    RosterProgram *Raw   = nullptr;
    const int      Count = proc_roster(&Raw, nullptr);
    if (Count < 0) return std::unexpected(Count);
    const std::unique_ptr<RosterProgram, void (*)(void *)> Held(Raw, free);
    std::vector<ProgramSeen> Programs;
    Programs.reserve(static_cast<std::size_t>(Count));
    for (int Index = 0; Index < Count; Index++) {
        const RosterProgram &One = Raw[Index];
        ProgramSeen          Seen{.Pid               = One.pid,
                                  .Generation        = One.generation,
                                  .SpawnerPid        = One.spawner_pid,
                                  .SpawnerGeneration = One.spawner_generation,
                                  .File              = One.program_file,
                                  .BeganUs           = One.began_us,
                                  .MemoryBytes       = One.memory_bytes,
                                  .Raised            = One.raised,
                                  .Places            = 0,
                                  .Strands           = {},
                                  .Tags              = One.tags ? One.tags : ""};
        Seen.Strands.reserve(One.strand_count);
        for (std::uint32_t Strand = 0; Strand < One.strand_count; Strand++) {
            const RosterStrand &S = One.strands[Strand];
            Seen.Strands.push_back(StrandSeen{S.pid, S.generation, S.state, S.core, S.cpu_us});
        }
        Programs.push_back(std::move(Seen));
    }
    return Programs;
}

// Where the display has each program, said onto the programs; false when no
// display answered.
bool ReadPlaces(std::vector<ProgramSeen> &Programs)
{
    std::vector<DisplayPlace> Places(64);
    std::uint32_t             Count = 0;
    for (;;) {
        const int Rc = display_places(Places.data(), static_cast<std::uint32_t>(Places.size()), &Count);
        if (Rc == OK) break;
        if (Rc != -ERR_BUFFER_TOO_SMALL || Places.size() >= 65535u) return false;
        Places.resize(std::min<std::size_t>(Places.size() * 4, 65535u));
    }
    for (std::uint32_t Index = 0; Index < Count; Index++)
        for (ProgramSeen &One : Programs)
            if (One.Pid == Places[Index].pid && One.Generation == Places[Index].generation)
                One.Places = static_cast<std::uint8_t>(One.Places | Places[Index].place);
    return true;
}

std::string ReadContext()
{
    for (;;) {
        std::size_t Needed = 0, Again = 0;
        if (use_get(nullptr, 0, &Needed) < 0 || Needed <= 1) return {};
        std::string Tags(Needed, '\0');
        if (use_get(Tags.data(), Tags.size(), &Again) > 0 && Again <= Needed) {
            Tags.resize(std::strlen(Tags.c_str()));
            return Tags;
        }
        if (Again <= Needed) return {};
    }
}

}  // namespace

std::uint64_t ProgramSeen::CpuUs() const noexcept
{
    std::uint64_t Sum = 0;
    for (const StrandSeen &One : Strands) Sum += One.CpuUs;
    return Sum;
}

std::expected<Reading, int> ReadMachine()
{
    Reading Now;
    Now.AtUs = clock_uptime_us();
    std::expected<std::vector<ProgramSeen>, int> Programs = ReadRoster();
    if (!Programs) return std::unexpected(Programs.error());
    Now.Programs    = std::move(*Programs);
    Now.PlacesKnown = ReadPlaces(Now.Programs);

    system_info_t Info{};
    if (sysinfo(&Info) == 0) {
        Now.MemoryTotal = Info.total_memory;
        Now.MemoryUsed  = Info.used_memory;
        Now.Cores.reserve(Info.cpu_total);
        for (std::uint32_t Core = 0; Core < Info.cpu_total; Core++) {
            system_core_usage_t Usage{};
            if (system_core_usage(Core, &Usage) != 0) break;
            Now.Cores.push_back(CoreSeen{Usage.cpu_busy_tsc, Usage.cpu_total_tsc, Usage.parked != 0,
                                         Usage.kernel != 0});
        }
    }
    system_resources_t Resources{};
    if (system_resources(&Resources) == 0 && Resources.volume_present) {
        Now.VolumePresent = true;
        const char *Name = Resources.volume_name;
        Now.VolumeName.assign(Name, static_cast<std::size_t>(
                                        std::find(Name, Name + sizeof(Resources.volume_name), '\0') - Name));
        Now.VolumeTotal = Resources.volume_total_bytes;
        Now.VolumeUsed  = Resources.volume_used_bytes;
    }
    Now.Context = ReadContext();
    return Now;
}

}  // namespace taskboard
