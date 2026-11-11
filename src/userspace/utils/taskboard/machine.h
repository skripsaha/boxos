/* machine.h — one reading of the machine: every program aboard with its
 * strands and where the display has it, each core's counters, memory, the
 * volume and the Use Context. What the system says, as it says it; nothing of
 * the screen. */
#ifndef TASKBOARD_MACHINE_H
#define TASKBOARD_MACHINE_H

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "box/types.h"

namespace taskboard {

struct StrandSeen {
    std::uint32_t Pid = 0;
    std::uint32_t Generation = 0;
    std::uint32_t State = 0;      // process_state_t
    std::uint32_t Core = 0;
    std::uint64_t CpuUs = 0;
};

// Where the display has a program (box/display.h DISPLAY_PLACE_*), as many
// as hold: shown, let go to live behind, keeping others.
enum Place : std::uint8_t { PlaceShown = 1, PlaceBehind = 2, PlaceKeeper = 4 };

struct ProgramSeen {
    std::uint32_t           Pid = 0;              // its main strand
    std::uint32_t           Generation = 0;
    std::uint32_t           SpawnerPid = 0;       // the program it was started from; 0 the system
    std::uint32_t           SpawnerGeneration = 0;
    file_id_t               File = 0;             // 0 none
    std::uint64_t           BeganUs = 0;          // the uptime it began at
    std::uint64_t           MemoryBytes = 0;
    std::uint32_t           Raised = 0;           // times raised again after a fall
    std::uint8_t            Places = 0;           // Place bits
    std::vector<StrandSeen> Strands;              // its main strand first
    std::string             Tags;                 // comma-joined

    std::uint64_t CpuUs() const noexcept;         // all its strands'
};

struct CoreSeen {
    std::uint64_t Busy = 0;      // TSC ticks, counted since boot
    std::uint64_t Total = 0;
    bool          Parked = false;
    bool          Kernel = false;   // a K-Core: the system's work, not programs
};

struct Reading {
    std::uint64_t            AtUs = 0;            // the uptime it was taken at
    std::vector<ProgramSeen> Programs;
    bool                     PlacesKnown = false; // the display answered where they are
    std::vector<CoreSeen>    Cores;
    std::uint64_t            MemoryTotal = 0;
    std::uint64_t            MemoryUsed = 0;
    bool                     VolumePresent = false;
    std::string              VolumeName;
    std::uint64_t            VolumeTotal = 0;
    std::uint64_t            VolumeUsed = 0;
    std::string              Context;             // the Use Context's tags; "" none
};

// The machine as it is now; a -error_t when the roster could not be read.
std::expected<Reading, int> ReadMachine();

}  // namespace taskboard

#endif
