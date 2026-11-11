/* owners.h — who holds memory, made of one reading: the owners of the whole of
 * memory with how much each holds, or of the address space with what the
 * registry says of each part; what each held when the map opened and has
 * gained since; the colors and marks they are drawn in. Nothing of the screen
 * beyond a color and a character. */
#ifndef MEMMAP_OWNERS_H
#define MEMMAP_OWNERS_H

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "box/types.h"

#include "look.h"
#include "machine.h"

namespace memmap {

struct Owner {
    Kind          What = Kind::Untold;
    std::string   Key;                  // the same owner from one reading to the next
    std::string   Name;
    std::string   Tags;                 // what typing is matched against, besides the name
    std::string   Note;                 // what this memory is, said once
    std::uint64_t Bytes = 0;
    std::int64_t  Since = 0;            // held more (or less) than when the map opened
    std::uint64_t Glow = 0;             // grown just now, by this much
    Color         Ink;
    char          Mark = ' ';           // drawn on a screen that has no pixels
    std::uint32_t Pid = 0;              // of a program; 0 for a kind
    file_id_t     File = 0;
    std::uint64_t BeganUs = 0;

    bool IsProgram() const noexcept { return What == Kind::Program && Pid != 0; }
    // Whether Part, in any case, is in its name or tags.
    bool Holds(std::string_view Part) const noexcept;
};

// A run of address space whose owner is Region's: where two regions lie over
// one another the one that says more wins. Devices beyond the last of memory
// are left out.
struct Segment {
    std::uint64_t     Base = 0;
    std::uint64_t     Bytes = 0;
    const RegionSeen *Region = nullptr;
};

std::vector<Segment> Resolve(const std::vector<RegionSeen> &Regions, std::uint64_t Span);

// What a kind of memory is called.
const char *KindName(Kind What) noexcept;

// What each owner of the whole of memory holds: each program, the kernel's
// image, the rest of the kernel's memory, rings, shared windows, and the free.
std::vector<Owner> MemoryOwners(const Reading &Now, const Look &Shown);
// What the registry tells of each part of the address space.
std::vector<Owner> AddressOwners(const Reading &Now, const Look &Shown, const std::vector<Segment> &Parts);
// Programs as one kind, when the person set it so.
void               Group(std::vector<Owner> &Owners, const Look &Shown);
void               Order(std::vector<Owner> &Owners, SortBy By);
// The index of the owner the segment belongs to, or -1.
std::ptrdiff_t     OwnerOfRegion(const std::vector<Owner> &Owners, const RegionSeen &Region);

// What each owner held when the map opened and just before: kept from one
// reading to the next, so the owners it is given say what they gained.
class Ledger {
public:
    void Take(std::vector<Owner> &Owners, std::uint64_t NowUs);

private:
    struct Account {
        std::uint64_t Opened = 0;
        std::uint64_t Before = 0;
        std::uint64_t Grown = 0;
        std::uint64_t GrownAtUs = 0;
    };
    std::unordered_map<std::string, Account> Accounts;
};

}  // namespace memmap

#endif
