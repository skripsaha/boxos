/* builtins.cpp — the table of the hub's own commands, and the small ones:
 * run, clear and exit. help, use and said have files of their own. */

#include "builtins.h"

#include <array>
#include <optional>
#include <string>

#include "box/core/cabin.h"
#include "box/memory.h"
#include "box/print.h"
#include "box/system.h"

#include "asking.h"
#include "hub.h"
#include "itinerary.h"
#include "launch.h"
#include "programs.h"
#include "say.h"
#include "words.h"

namespace hub {

namespace {

// run — a program (or an Itinerary) given as a thing rather than by name:
// `run planets.elf`, `run it`. The rest of the line is the program's own,
// sorted into its Form like the words after a program's name.
bool Run(Hub &Self, const Words &Typed)
{
    if (Typed.Count() < 2) {
        Say("run: run what? A file such as planets.elf, or it\n");
        return false;
    }
    const programs::Picked Program = programs::PickThing(Self, Typed[1]);
    if (!Program) return false;
    const std::optional<std::string> Name = programs::NameOf(*Program);
    const std::string_view           Who  = Name ? std::string_view(*Name) : Typed[1];
    return programs::IsItinerary(*Program) ? itinerary::Run(Self, *Program, Who, Typed.Tail(2))
                                           : launch::Program(Self, *Program, Who, Typed.Tail(2));
}

bool Clear(Hub &, const Words &)
{
    clear();
    return true;
}

// Whether another hub is there to type at once this one has gone.
bool AnotherHub()
{
    ProcMate     *Mates = nullptr;
    std::uint32_t Total = 0;
    int           Delivered = proc_crew("hub", &Mates, &Total);
    if (Delivered < 0) return true;   // not known: the person is not asked over nothing
    const std::uint32_t Me    = cabin_info()->pid;
    bool                Other = Total > static_cast<std::uint32_t>(Delivered);
    for (int I = 0; I < Delivered; I++) Other |= Mates[I].pid != Me;
    free(Mates);
    return Other;
}

// exit — the hub ends. When it is the last one, the person is asked first:
// with no hub left, nothing on this machine takes a typed line.
bool Exit(Hub &Self, const Words &)
{
    bool Sure = true;
    if (!AnotherHub() && (asking::YesNo("no hub will be left — sure?", Sure) != asking::Answer::Given || !Sure)) {
        Say("exit: the hub stays\n");
        return true;
    }
    Self.Going = false;
    return true;
}

constexpr std::array<Builtin, 6> Table{{
    {"help", "help [NAME]", "what there is: these commands, or what a program takes and how it is set",
     Takes::Command, Help},
    {"use", "use [tags] [+tag] [-tag]", "set, change or clear the Use Context", Takes::Tags, Use},
    {"said", "said [before] [NAME]", "what this machine said (into the file NAME when one is given)", Takes::Nothing,
     Said},
    {"run", "run THING", "start a program given as a file, or it", Takes::Program, Run},
    {"clear", "clear", "clear the screen", Takes::Nothing, Clear},
    {"exit", "exit", "end this hub", Takes::Nothing, Exit},
}};

}  // namespace

std::span<const Builtin> Builtins()
{
    return Table;
}

const Builtin *BuiltinNamed(std::string_view Name)
{
    for (const Builtin &One : Table)
        if (One.Name == Name) return &One;
    return nullptr;
}

}  // namespace hub
