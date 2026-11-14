/* programs.h — find the program a name or a thing means.
 *
 * The rule is the kernel's (SysExecCandidate): a file counts as a program when
 * its bytes are a box or an ELF image — what it holds, not the tags it wears.
 * The hub holds the files that carry the name to the same rule before it asks
 * the kernel to start one, so what it lists is exactly what would start. A
 * file that begins as an Itinerary does (itinerary.h) is a command too: the
 * hub runs its steps itself. */
#ifndef HUB_PROGRAMS_H
#define HUB_PROGRAMS_H

#include <cstddef>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "box/types.h"

#include "look.h"

namespace hub {

struct Hub;

namespace programs {

bool IsProgram(file_id_t File);
bool IsItinerary(file_id_t File);

// The file's name; nothing when it cannot be read.
std::optional<std::string> NameOf(file_id_t File);

// How much of a file's name is the name it is called by: up to its last dot,
// as the volume stamps it.
std::size_t StemLength(std::string_view FileName);

// Every command on the volume — each program's and Itinerary's file — once,
// with its file's name.
void EachCommand(const std::function<void(file_id_t File, std::string_view FileName)> &Visit);

// How a pick ended when it found nothing to start: the person was told why
// (Said), or the volume's refusal as a negative -error_t, said by the caller.
inline constexpr int Said = 1;
using Picked = std::expected<file_id_t, int>;

// The program a command's name means: the files wearing the tag NAME that are
// programs or Itineraries (the name is how the person finds a file; whether it
// is one is what it holds). One is taken; of several the person is asked
// which, as Shown says; none is -ERR_FILE_NOT_FOUND.
Picked Pick(const Look &Shown, std::string_view Name);

// The program a thing typed means: a file here (or else anywhere) by that
// name, or `it` / `them`, which must be a program. Asked which when it names
// several; said what is wrong when it names no program.
Picked PickThing(Hub &Self, std::string_view Thing);

}  // namespace programs
}  // namespace hub

#endif
