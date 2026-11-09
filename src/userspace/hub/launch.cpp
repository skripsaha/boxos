/* launch.cpp — a filled Form started as the program lives, its end heard, its
 * gift kept. */

#include "launch.h"

#include <optional>
#include <utility>

#include "box/life.h"

#include "fill.h"
#include "hub.h"
#include "news.h"
#include "remake.h"
#include "say.h"

namespace hub::launch {

namespace {

// How the program lives (box/life.h): a task when it says nothing, or when
// what it says cannot be read — the hub then stands by, as for any task.
ProgramLiving LivingOf(file_id_t Program)
{
    ProgramLife Life{};
    return program_life(Program, &Life) == 0 ? static_cast<ProgramLiving>(Life.Living) : PROGRAM_TASK;
}

// The kernel would not start it, and why.
void SayRefused(std::string_view Who, int Rc)
{
    Say("{}: is there and would not start ({})\n", Who, Reason(Rc));
}

Start StartOf(const fill::Filled &What, std::string_view Who, ProgramLiving Living)
{
    return Start{What.Program, std::string(Who), What.Luggage, Living, What.Screen, What.Souvenir};
}

// A program the prompt stood by for, settled: let go to live behind, which is
// said; or ended, how said when not well, and what it gave kept as `them`.
// True when it lives on, or ended with 0.
bool Settle(Hub &Self, std::string_view Who, std::expected<Settled, int> Ran)
{
    if (!Ran) {
        SayRefused(Who, Ran.error());
        return false;
    }
    if (Ran->LetGo) {
        news::LetGo(Self.Shown, Who);
        return true;
    }
    news::Ended(Self.Shown, Who, Ran->End);
    if (Ran->End.Kept) Self.Given.Keep(std::move(*Ran->End.Kept));
    return Ran->End.Seen && Ran->End.Code == 0;
}

}  // namespace

bool Program(Hub &Self, file_id_t Program, std::string_view Who, std::string_view Line)
{
    if (!remake::WhenChanged(Self, Program, Who)) return false;
    const std::optional<fill::Filled> What = fill::FromLine(Self, Program, Who, Line);
    if (!What) return false;

    const ProgramLiving Living = LivingOf(Program);
    if (Living == PROGRAM_TASK) return Settle(Self, Who, Self.Started.InFront(StartOf(*What, Who, Living)));
    if (const std::expected<void, int> Started = Self.Started.Behind(StartOf(*What, Who, Living)); !Started) {
        SayRefused(Who, Started.error());
        return false;
    }
    return true;
}

bool OnThing(Hub &Self, file_id_t Program, std::string_view Who, file_id_t Thing, file_id_t &Given)
{
    Given = 0;
    const std::optional<fill::Filled> What = fill::OnThing(Program, Who, Thing);
    if (!What) return false;
    // What it gives is wanted now, so it is stood by for however it lives.
    std::expected<Settled, int> Ran = Self.Started.InFront(StartOf(*What, Who, PROGRAM_TASK));
    if (!Ran) {
        SayRefused(Who, Ran.error());
        return false;
    }
    if (Ran->LetGo) {
        news::LetGo(Self.Shown, Who);
        return false;
    }
    news::Ended(Self.Shown, Who, Ran->End);
    if (!Ran->End.Seen || Ran->End.Code != 0) return false;
    if (Ran->End.Kept && !Ran->End.Kept->empty()) Given = Ran->End.Kept->front();
    return true;
}

bool Forward(Hub &Self, const BehindNow &Which)
{
    return Settle(Self, Which.Who, Self.Started.Forward(Which.Pid, Which.Generation));
}

}  // namespace hub::launch
