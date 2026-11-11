/* journal.cpp — what happened, heard and said. */

#include "journal.h"

#include <algorithm>
#include <cstring>
#include <format>

#include "box/clock.h"
#include "box/system.h"
#include "proc_exit.h"

namespace taskboard {

namespace {

// So many entries are kept; the oldest go first.
constexpr std::size_t JournalMost = 200;
// A birth no reading found after so many is let go: it ended between them.
constexpr std::uint32_t BirthPatience = 2;

TouchTag Claimed(const char *Name)
{
    const TouchTag Tag = touch_pair_choose(touch_intern(Name));
    if (Tag == TOUCH_TAG_INVALID || touch_claim(Tag, TOUCH_REST, 0, 0) != 0) return TOUCH_TAG_INVALID;
    return Tag;
}

std::string Ordinal(std::uint32_t N)
{
    const char *Suffix = (N % 100u >= 11 && N % 100u <= 13) ? "th"
                       : N % 10u == 1                        ? "st"
                       : N % 10u == 2                        ? "nd"
                       : N % 10u == 3                        ? "rd"
                                                             : "th";
    return std::format("{}{}", N, Suffix);
}

}  // namespace

std::string TimeOfDay()
{
    BoxTime Now{};
    if (clock_boxtime_local(&Now) == 0) return std::format("{:02}:{:02}:{:02}", Now.hour, Now.minute, Now.second);
    const std::uint64_t Seconds = clock_uptime_ms() / 1000u;
    return std::format("+{:02}:{:02}", Seconds / 60u, Seconds % 60u);
}

Journal::Journal() : Deaths(Claimed(TOUCH_TAG_PROCESS_DIED)), Births(Claimed(TOUCH_TAG_PROCESS_SPAWNED)) {}

Journal::~Journal()
{
    if (Deaths != TOUCH_TAG_INVALID) touch_release(Deaths);
    if (Births != TOUCH_TAG_INVALID) touch_release(Births);
}

bool Journal::Hears() const noexcept
{
    return Deaths != TOUCH_TAG_INVALID && Births != TOUCH_TAG_INVALID;
}

void Journal::Say(Happened What, std::string Text)
{
    Kept.push_back(Entry{TimeOfDay(), What, std::move(Text)});
    while (Kept.size() > JournalMost) Kept.pop_front();
}

void Journal::Listen(const Programs &Known)
{
    Touch Heard{};
    while (Births != TOUCH_TAG_INVALID && touch_try_pop_tag(Births, &Heard)) {
        TouchProcessSpawned Born{};
        if (Heard.payload_len < sizeof(Born)) continue;
        std::memcpy(&Born, Heard.payload, sizeof(Born));
        Pending.push_back(Birth{Born.pid, Born.generation, Born.parent_program_pid, Born.parent_program_generation, 0});
    }
    while (Deaths != TOUCH_TAG_INVALID && touch_try_pop_tag(Deaths, &Heard)) {
        TouchProcessDied Died{};
        if (Heard.payload_len < 12) continue;
        std::memcpy(&Died, Heard.payload, std::min<std::size_t>(Heard.payload_len, sizeof(Died)));
        if (Heard.payload_len < sizeof(Died)) Died.fell = 0;
        // A program's end is its main strand's; another strand's end is the
        // program's own affair.
        const Program *Who = Known.Recall(Died.pid, Died.generation);
        if (!Who) continue;
        Died.in[sizeof(Died.in) - 1] = '\0';
        const std::int32_t Code = Died.exit_code;
        const std::string  In(Died.in);
        if (Code == PROC_EXIT_STACK_OVERFLOW) {
            Say(Happened::Fell, std::format("{} fell: its stack overflowed", Who->Name));
        } else if (Code == PROC_EXIT_CRASHED) {
            const char *Fault = Died.fell ? process_fault_said(Died.vector) : "it was torn down";
            Say(Happened::Fell, Died.fell && !In.empty() ? std::format("{} fell in {}: {}", Who->Name, In, Fault)
                                                         : std::format("{} fell: {}", Who->Name, Fault));
        } else if (Code == PROC_EXIT_KILLED) {
            Say(Happened::Ended, std::format("{} was ended", Who->Name));
        } else if (Code == 0) {
            Say(Happened::Ended, std::format("{} ended", Who->Name));
        } else {
            Say(Happened::Ended, std::format("{} ended, saying {}", Who->Name, Code));
        }
    }
}

void Journal::Take(const Programs &Known)
{
    std::erase_if(Pending, [&](Birth &One) {
        const Program *Who = Known.Find(One.Pid, One.Generation);
        if (!Who) return ++One.Readings > BirthPatience;
        if (One.FromPid == 0 && Who->Seen.Raised > 0) {
            Say(Happened::Raised, std::format("{} raised again after a fall, the {} time", Who->Name,
                                              Ordinal(Who->Seen.Raised)));
            return true;
        }
        const Program *From = Known.Find(One.FromPid, One.FromGeneration);
        Say(Happened::Began, From       ? std::format("{} started from {}", Who->Name, From->Name)
                             : One.FromPid ? std::format("{} started", Who->Name)
                                           : std::format("{} started by the system", Who->Name));
        return true;
    });
}

}  // namespace taskboard
