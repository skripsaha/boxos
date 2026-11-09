/* line.cpp — a line cut into words and handed to what its first word names. */

#include "line.h"

#include <new>
#include <optional>
#include <string>

#include "box/error.h"

#include "builtins.h"
#include "hub.h"
#include "itinerary.h"
#include "launch.h"
#include "nearest.h"
#include "programs.h"
#include "say.h"
#include "words.h"

namespace hub {

namespace {

// A program by its name: the programs the name means are found the way the
// kernel would find them, the person asked which when there are several, and
// the words after the name sorted into its Form — or an Itinerary's, whose
// steps then run.
bool RunProgram(Hub &Self, const Words &Cut)
{
    const std::string_view Name    = Cut[0];
    const programs::Picked Program = programs::Pick(Self.Shown, Name);
    if (Program)
        return programs::IsItinerary(*Program) ? itinerary::Run(Self, *Program, Name, Cut.Tail(1))
                                               : launch::Program(Self, *Program, Name, Cut.Tail(1));
    if (Program.error() == programs::Said) return false;

    // Which of the two things happened is said: a name nobody has, or a
    // program that is there and would not start — its medium gone, most
    // often, which the person must not be told is a command that does not
    // exist.
    if (box_errno_of(Program.error()) != ERR_FILE_NOT_FOUND) {
        SayIn(Self.Shown.RefusalColor, "{}: is there and would not start\n", Name);
        return false;
    }
    std::string Said = "Unknown command: " + std::string(Name);
    if (Self.Shown.DidYouMean)
        if (const std::optional<std::string> Close = nearest::For(Name)) Said += "\n" + *Close;
    SayIn(Self.Shown.RefusalColor, Said + "\n");
    return false;
}

}  // namespace

bool RunLine(Hub &Self, std::string_view Line)
{
    try {
        const Words Cut(Line);
        if (Cut.Empty()) return true;
        if (const Builtin *Own = BuiltinNamed(Cut[0])) return Own->Run(Self, Cut);
        return RunProgram(Self, Cut);
    } catch (const std::bad_alloc &) {
        SayIn(Self.Shown.RefusalColor, "hub: there is no memory to run that line\n");
        return false;
    }
}

}  // namespace hub
