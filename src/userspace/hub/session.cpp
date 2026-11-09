/* session.cpp — the prompt, the line typed at it, the line kept and run. */

#include "session.h"

#include <chrono>
#include <new>
#include <optional>
#include <string>

#include "box/color.h"
#include "box/error.h"
#include "box/memory.h"
#include "box/print.h"
#include "box/settings.h"
#include "box/cxx/settings.h"

#include "builtins.h"
#include "context.h"
#include "fill.h"
#include "hub.h"
#include "itinerary.h"
#include "nearest.h"
#include "programs.h"
#include "prompt.h"
#include "whisper.h"
#include "words.h"

namespace hub {

namespace {

void Refuse(const Hub &Self, const std::string &Said)
{
    set_color(Self.Shown.RefusalColor);
    print_bytes(Said.data(), Said.size());
    print("\n");
    set_color(COLOR_DEFAULT);
}

// A program by its name: the programs the name means are found the way the
// kernel would find them, the person asked which when there are several, and
// the words after the name sorted into its Form — or an Itinerary's, whose
// steps then run.
bool RunProgram(Hub &Self, std::string_view Name, std::string_view Line)
{
    programs::Picked Program = programs::Pick(Name);
    if (Program) {
        const std::string_view Rest = hub::Rest(Line, 1);
        return programs::IsItinerary(*Program) ? itinerary::Run(Self, *Program, Name, Rest)
                                               : fill::AndRun(Self, *Program, Name, Rest);
    }
    if (Program.error() == programs::Said) return false;

    // Which of the two things happened is said: a name nobody has, or a
    // program that is there and would not start — its medium gone, most
    // often, which the person must not be told is a command that does not
    // exist.
    if (box_errno_of(Program.error()) != ERR_FILE_NOT_FOUND) {
        Refuse(Self, std::string(Name) + ": is there and would not start");
        return false;
    }
    std::string Said = "Unknown command: " + std::string(Name);
    if (Self.Shown.DidYouMean)
        if (std::optional<std::string> Close = nearest::For(Name)) Said += "\n" + *Close;
    Refuse(Self, Said);
    return false;
}

// "took 12 s", "took 3 min 20 s", "took 1 h 2 min".
std::string Took(std::chrono::steady_clock::duration Spent)
{
    const auto Seconds = std::chrono::duration_cast<std::chrono::seconds>(Spent).count();
    if (Seconds < 60) return "took " + std::to_string(Seconds) + " s";
    if (Seconds < 3600)
        return "took " + std::to_string(Seconds / 60) + " min " + std::to_string(Seconds % 60) + " s";
    return "took " + std::to_string(Seconds / 3600) + " h " + std::to_string(Seconds % 3600 / 60) + " min";
}

// The line typed at the prompt, kept, run, and how long it took said when it
// took long.
void Typed(Hub &Self, std::string_view Line, std::string_view Context)
{
    Self.Lines.Keep(Line, Context, Self.Shown.History);
    const auto Began = std::chrono::steady_clock::now();
    RunLine(Self, Line);
    const auto Spent = std::chrono::steady_clock::now() - Began;
    if (Self.Shown.LongCommand > 0 && Spent >= std::chrono::seconds(Self.Shown.LongCommand)) {
        set_color(Self.Shown.NewsColor);
        print((Took(Spent) + "\n").c_str());
        set_color(COLOR_DEFAULT);
    }
}

}  // namespace

bool RunLine(Hub &Self, std::string_view Line)
{
    try {
        const Words Cut(Line);
        if (Cut.Empty()) return true;
        if (const Builtin *Own = BuiltinNamed(Cut[0])) return Own->Run(Self, Cut, Line);
        return RunProgram(Self, Cut[0], Line);
    } catch (const std::bad_alloc &) {
        Refuse(Self, "hub: there is no memory to run that line");
        return false;
    }
}

bool Converse(Hub &Self)
{
    box::settings::watch Watch;
    while (Self.Going) {
        if (Watch.changed()) Self.Shown = ReadLook();
        const std::string Context = context::Whole().value_or(std::string());
        ShowPrompt(Self, Context);

        // The line grows as it is typed: no ceiling but memory. The Whisper
        // sees the volume as it is when this line begins.
        char *Input  = nullptr;
        int   Length = 0;
        {
            Whisper Hints(Self, Context);
            Length = readline_whispered(&Input, Hints.Hook());
        }
        if (Length < 0) return false;
        if (Length > 0) Typed(Self, std::string_view(Input, static_cast<std::size_t>(Length)), Context);
        free(Input);
    }
    return true;
}

}  // namespace hub
