/* machine.h — one reading of the machine's memory: how much there is and is in
 * use, what each program has mapped for it, and every region the registry of
 * tagged memory (MemTag) tells of. What the system says, as it says it;
 * nothing of the screen. */
#ifndef MEMMAP_MACHINE_H
#define MEMMAP_MACHINE_H

#include <cstdint>
#include <expected>
#include <string>
#include <unordered_map>
#include <vector>

#include "box/types.h"

namespace memmap {

// What memory is, by who holds it: the kernel's image and the rest of its
// own, the rings programs talk through, windows programs share, devices and
// firmware, a program's, nobody's, and what is in use that nothing says whose.
enum class Kind : std::uint8_t { Image, Kernel, Rings, Shared, Device, Program, Free, Untold };

struct ProgramSeen {
    std::uint32_t Pid = 0;            // its main strand
    std::uint32_t Generation = 0;
    file_id_t     File = 0;           // 0 none
    std::uint64_t BeganUs = 0;
    std::uint64_t Bytes = 0;          // memory mapped for it
    std::string   Name;
    std::string   Tags;               // comma-joined
};

// A run of address space the registry tells of: of the kinds Image, Rings,
// Shared and Device by what its tags say, and Untold for the memory a zone
// holds that no tag speaks for.
struct RegionSeen {
    std::uint64_t Base = 0;
    std::uint64_t Bytes = 0;
    Kind          What = Kind::Untold;
    std::uint32_t Cabin = 0;          // the program whose rings these are; 0 none
    std::string   Tags;               // comma-joined
};

struct Reading {
    std::uint64_t            AtUs = 0;            // the uptime it was taken at
    std::uint64_t            Total = 0;           // bytes of memory
    std::uint64_t            Used = 0;
    std::vector<ProgramSeen> Programs;
    std::vector<RegionSeen>  Regions;
    std::uint64_t            Span = 0;            // the end of the highest zone of memory: the map's address space
    bool                     RegionsKnown = false;
};

class Machine {
public:
    // The machine as it is now; a -error_t when the roster could not be read.
    std::expected<Reading, int> Read();

private:
    struct Cached {
        std::uint32_t Generation = 0;
        RegionSeen    Region;
        bool          Kept = false;      // it is one the map shows
    };

    std::string NameOf(file_id_t File, const std::string &FirstTag);
    bool        ReadRegions();

    std::unordered_map<file_id_t, std::string> Names;
    std::unordered_map<std::uint32_t, Cached>  Seen;     // by region id
    std::vector<RegionSeen>                    Regions;
    std::uint64_t                              RegionGeneration = 0;
    std::uint64_t                              BitmapGeneration = 0;
    bool                                       Scanned = false;
};

}  // namespace memmap

#endif
