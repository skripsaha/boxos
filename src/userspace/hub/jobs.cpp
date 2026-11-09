/* jobs.cpp — a program started, and its end waited for as an event. */

#include "jobs.h"

#include <cstring>

#include "box/color.h"
#include "box/error.h"
#include "box/form.h"
#include "box/memory.h"
#include "box/print.h"
#include "box/system.h"
#include "proc_exit.h"

#include "hub.h"
#include "say.h"

namespace hub {

void Jobs::Begin()
{
    Died = touch_pair_choose(touch_intern(TOUCH_TAG_PROCESS_DIED));
    if (Died != TOUCH_TAG_INVALID && touch_claim(Died, TOUCH_REST, 0, 0) != 0) Died = TOUCH_TAG_INVALID;
}

// The deaths heard while nothing was started: none of them is a child that
// is still to be started, and a starting program's own end must not wait
// behind them.
void Jobs::Forget()
{
    Touch Stale{};
    while (Died != TOUCH_TAG_INVALID && touch_try_pop_tag(Died, &Stale)) {}
}

// Its end, as the kernel says it: the death of its main strand — the last a
// program announces — in this incarnation, said by the kernel and no one else.
// With no ear for deaths, the end is asked of the kernel's record instead.
Ending Jobs::WaitGone(std::uint32_t Pid, std::uint32_t Generation)
{
    Ending End;
    if (Died == TOUCH_TAG_INVALID) {
        End.Seen = process_gone(Pid, Generation, &End.Code) == 0;
    } else {
        for (;;) {
            Touch Heard{};
            if (touch_await(Died, &Heard, 0) != 0) break;
            if (!(Heard.flags & TOUCH_FLAG_KERNEL) || Heard.payload_len < sizeof(TouchProcessDied)) continue;
            TouchProcessDied Death;
            std::memcpy(&Death, Heard.payload, sizeof(Death));
            if (Death.pid != Pid || Death.generation != Generation) continue;
            End.Seen = true;
            End.Code = Death.exit_code;
            break;
        }
    }

    // What it queued is read off whether it is kept or not, so no stale gift
    // is met later.
    file_id_t *Ids   = nullptr;
    size_t     Count = 0;
    End.Collected    = souvenir_collect(Pid, &Ids, &Count) == 0;
    if (End.Collected && Ids) End.Given.assign(Ids, Ids + Count);
    free(Ids);
    return End;
}

std::expected<Ending, int> Jobs::Run(file_id_t Program, std::string_view Luggage)
{
    Forget();
    std::uint32_t Generation = 0;
    int           Pid = proc_exec_file(Program, Luggage.data(), Luggage.size(), nullptr, &Generation);
    if (Pid <= 0) return std::unexpected(Pid == 0 ? -ERR_SPAWN_FAILED : Pid);
    return WaitGone(static_cast<std::uint32_t>(Pid), Generation);
}

void SayEnding(const Hub &Self, std::string_view Who, const Ending &End)
{
    if (!Self.Shown.Ended || !End.Seen || End.Code == 0) return;
    set_color(Self.Shown.NewsColor);
    if (End.Code > 0) Say("{} ended: {}\n", Who, static_cast<int>(End.Code));
    else if (End.Code == PROC_EXIT_KILLED) Say("{} was ended by another program\n", Who);
    else if (End.Code == PROC_EXIT_STACK_OVERFLOW) Say("{} stopped: its stack ran out\n", Who);
    else Say("{} stopped: it faulted\n", Who);
    set_color(COLOR_DEFAULT);
}

}  // namespace hub
