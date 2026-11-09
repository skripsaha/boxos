/* jobs.cpp — the strand that keeps the hub's programs, and what the prompt's
 * strand asks of it. */

#include "jobs.h"

#include <algorithm>
#include <cstring>
#include <thread>
#include <utility>

#include "box/core/cabin.h"
#include "box/display.h"
#include "box/error.h"
#include "box/form.h"
#include "box/memory.h"
#include "box/system.h"
#include "box/turnin.h"

#include "news.h"
#include "say.h"

namespace hub {

namespace {

// The tag a hub's prompt wakes its keeping strand on: one for every hub, the
// payload the pid of the hub it is for.
constexpr const char *WakeTagName = "hub:wake";

// What of what a program gave back is kept as `them`: the file its screen
// went into, or its Souvenir when it was asked for and could be read off.
std::optional<std::vector<file_id_t>> KeptOf(const Start &What, bool Collected, std::vector<file_id_t> Given)
{
    if (What.Screen != 0) return std::vector<file_id_t>{What.Screen};
    if (What.Souvenir && Collected) return Given;
    return std::nullopt;
}

}  // namespace

bool Jobs::Begin(const Look &Shown)
{
    Seen    = Shown;
    WakeTag = touch_intern(WakeTagName);
    bool Ready = false, Heard = false;
    try {
        std::thread([this, &Ready, &Heard] {
            // Claimed on the keeping strand itself, before the first start:
            // no death can come before its ear.
            Deaths = touch_pair_choose(touch_intern(TOUCH_TAG_PROCESS_DIED));
            Woken  = touch_pair_choose(WakeTag);
            News   = touch_pair_choose(touch_intern(DISPLAY_FRONT_TAG));
            const bool Claimed = Deaths != TOUCH_TAG_INVALID && touch_claim(Deaths, TOUCH_REST, 0, 0) == 0 &&
                                 Woken != TOUCH_TAG_INVALID && touch_claim(Woken, TOUCH_REST, 0, 0) == 0;
            // The display's news is heard when there is a display to say it.
            if (News != TOUCH_TAG_INVALID && touch_claim(News, TOUCH_REST, 0, 0) != 0) News = TOUCH_TAG_INVALID;
            {
                std::lock_guard Held(Lock);
                Heard = Claimed;
                Ready = true;
            }
            Settle.notify_all();
            if (Claimed) Keep();
        }).detach();
    } catch (...) {
        Say("hub: no strand could be made to keep its programs; nothing can be started\n");
        return false;
    }
    std::unique_lock Held(Lock);
    Settle.wait(Held, [&] { return Ready; });
    if (!Heard) Say("hub: the ends of programs cannot be heard; nothing can be started\n");
    return Heard;
}

// ---- the keeping strand ------------------------------------------------------

// Everything it waits on looked at, and asleep when nothing came: the mark
// taken first, so whatever comes after it wakes the sleep. A start is looked
// at before the deaths, so the death of a program just started finds it kept.
void Jobs::Keep()
{
    for (;;) {
        const TurnInMark Mark  = box_mark();
        bool             Moved = HearWake();
        Moved |= StartAsked();
        Moved |= HearDeaths();
        Moved |= HearDisplay();
        if (!Moved) box_turn_in(Mark);
    }
}

bool Jobs::HearWake()
{
    bool  Heard = false;
    Touch Event{};
    while (touch_try_pop_tag(Woken, &Event)) Heard = true;
    return Heard;
}

// The next program the prompt asked for, started — the keeping strand's
// child — and kept by the display in front of the person, or behind. A
// machine with no display has nobody to keep it from, and nothing is lost.
bool Jobs::StartAsked()
{
    Asked *Next = nullptr;
    {
        std::lock_guard Held(Lock);
        if (Starts.empty()) return false;
        Next = Starts.front();
        Starts.pop_front();
    }
    std::uint32_t Generation = 0;
    const int     Pid = proc_exec_file(Next->What.Program, Next->What.Luggage.data(), Next->What.Luggage.size(),
                                       nullptr, &Generation);
    if (Pid > 0) {
        const auto    Started = static_cast<std::uint32_t>(Pid);
        std::uint32_t Display = 0;
        const int     Kept    = Next->Front ? display_front(Started, Generation, &Display)
                                            : display_back(Started, Generation, &Display);
        std::lock_guard Held(Lock);
        if (Kept == OK) DisplayPid = Display;
        Job One;
        One.What = Next->What;
        One.What.Luggage.clear();
        One.Pid        = Started;
        One.Generation = Generation;
        One.Front      = Next->Front;
        Running.push_back(std::move(One));
    }
    {
        std::lock_guard Held(Lock);
        Next->Rc         = Pid > 0 ? 0 : (Pid == 0 ? -ERR_SPAWN_FAILED : Pid);
        Next->Pid        = Pid > 0 ? static_cast<std::uint32_t>(Pid) : 0;
        Next->Generation = Generation;
        Next->Done       = true;
    }
    Settle.notify_all();
    return true;
}

bool Jobs::HearDeaths()
{
    bool  Heard = false;
    Touch Event{};
    while (touch_try_pop_tag(Deaths, &Event)) {
        Heard = true;
        if (!(Event.flags & TOUCH_FLAG_KERNEL) || Event.payload_len < sizeof(TouchProcessDied)) continue;
        TouchProcessDied Death;
        std::memcpy(&Death, Event.payload, sizeof(Death));
        Died(Death);
    }
    return Heard;
}

// A death — the main strand's, the last a program announces — of one of the
// hub's programs: its end and what it gave are read. The prompt's strand is
// told of the one in front; the end of one behind is said now.
void Jobs::Died(const TouchProcessDied &Death)
{
    std::unique_lock Held(Lock);
    const auto       It = std::find_if(Running.begin(), Running.end(), [&](const Job &One) {
        return One.Pid == Death.pid && One.Generation == Death.generation;
    });
    if (It == Running.end()) return;
    Job &One     = *It;
    One.Over     = true;
    One.End.Seen = true;
    One.End.Code = Death.exit_code;
    One.End.Fell = Death.fell != 0;
    if (One.End.Fell) {
        One.End.Vector    = Death.vector;
        const char *Named = std::find(Death.in, Death.in + sizeof(Death.in), '\0');
        One.End.In.assign(Death.in, static_cast<std::size_t>(Named - Death.in));
    }
    // What it queued is read off whether it is kept or not, so no stale gift
    // is met later. It came to this strand, the one that started it.
    file_id_t *Ids       = nullptr;
    size_t     Count     = 0;
    const bool Collected = souvenir_collect(One.Pid, &Ids, &Count) == 0;
    One.End.Kept = KeptOf(One.What, Collected, Collected && Ids ? std::vector<file_id_t>(Ids, Ids + Count)
                                                                : std::vector<file_id_t>());
    free(Ids);

    // The prompt stands by for it, or has not yet heard it was let go: the
    // prompt is told, and takes its end as that of one in front.
    if (One.Front || One.LetGo) {
        Held.unlock();
        Settle.notify_all();
        return;
    }
    const Job Ended = std::move(One);
    Running.erase(It);
    if (Ended.End.Kept && !Ended.End.Kept->empty()) Gift = Ended.End.Kept;
    const Look Shown = Seen;
    Held.unlock();
    news::Finished(Shown, Ended.What.Who, Ended.What.Living == PROGRAM_SERVES, Ended.End);
}

bool Jobs::HearDisplay()
{
    bool  Heard = false;
    Touch Event{};
    while (News != TOUCH_TAG_INVALID && touch_try_pop_tag(News, &Event)) {
        Heard = true;
        DisplayFrontNews Said;
        if (Event.payload_len < sizeof(Said) || DisplayPid == 0 || Event.source_pid != DisplayPid) continue;
        std::memcpy(&Said, Event.payload, sizeof(Said));
        if (Said.keeper_pid != cabin_info()->pid || Said.keeper_generation != cabin_info()->generation) continue;
        Pressed(Said.what, Said.pid, Said.generation);
    }
    return Heard;
}

// What the person pressed over a program the hub keeps in front, or what a
// program it keeps behind asked.
void Jobs::Pressed(std::uint8_t What, std::uint32_t Pid, std::uint32_t Generation)
{
    std::unique_lock Held(Lock);
    const auto       It = std::find_if(Running.begin(), Running.end(), [&](const Job &One) {
        return One.Pid == Pid && One.Generation == Generation && !One.Over;
    });
    if (It == Running.end()) return;
    Job &One = *It;

    if (What == DISPLAY_FRONT_END && One.Front) {
        // Asked first: a program that hears it ends as it chooses — edit asks
        // what to do with what is not saved. One none of whose strands reads,
        // or one asked already, ends at once.
        const bool Again = One.End.Asked;
        One.End.Asked    = true;
        Held.unlock();
        if (Again || display_ask_end(Pid, Generation) != 1) (void)proc_finish(Pid, Generation);
        return;
    }
    if (What == DISPLAY_FRONT_LET_GO && One.Front) {
        One.Front = false;
        One.LetGo = true;
        Held.unlock();
        std::uint32_t Display = 0;
        (void)display_back(Pid, Generation, &Display);
        Settle.notify_all();
        return;
    }
    if (What == DISPLAY_FRONT_WANTS && !One.Front && !One.Wants) {
        One.Wants             = true;
        const std::string Who = One.What.Who;
        const Look        Shown = Seen;
        Held.unlock();
        news::Wants(Shown, Who);
    }
}

// ---- what the prompt's strand asks -------------------------------------------

// A start handed to the keeping strand, and waited for until it is done.
std::expected<void, int> Jobs::Ask(Asked &One)
{
    {
        std::lock_guard Held(Lock);
        Starts.push_back(&One);
    }
    const std::uint32_t Me = cabin_info()->pid;
    (void)touch_send(WakeTag, &Me, sizeof(Me), 0);
    std::unique_lock Held(Lock);
    Settle.wait(Held, [&] { return One.Done; });
    if (One.Rc != 0) return std::unexpected(One.Rc);
    return {};
}

std::expected<Settled, int> Jobs::StandBy(std::uint32_t Pid, std::uint32_t Generation)
{
    std::unique_lock Held(Lock);
    const auto       Find = [&] {
        return std::find_if(Running.begin(), Running.end(),
                            [&](const Job &One) { return One.Pid == Pid && One.Generation == Generation; });
    };
    Settle.wait(Held, [&] {
        const auto It = Find();
        return It == Running.end() || It->Over || It->LetGo;
    });
    const auto It = Find();
    if (It == Running.end()) return std::unexpected(-ERR_PROCESS_NOT_FOUND);
    // An end comes first: one let go that ended before the prompt looked has
    // ended, and is not said to live on.
    Settled Done;
    if (!It->Over) {
        It->LetGo  = false;
        Done.LetGo = true;
        return Done;
    }
    Done.End = std::move(It->End);
    Running.erase(It);
    return Done;
}

std::expected<Settled, int> Jobs::InFront(Start What)
{
    Asked One;
    One.What  = std::move(What);
    One.Front = true;
    if (std::expected<void, int> Started = Ask(One); !Started) return std::unexpected(Started.error());
    return StandBy(One.Pid, One.Generation);
}

std::expected<void, int> Jobs::Behind(Start What)
{
    Asked One;
    One.What = std::move(What);
    return Ask(One);
}

std::expected<Settled, int> Jobs::Forward(std::uint32_t Pid, std::uint32_t Generation)
{
    {
        std::lock_guard Held(Lock);
        const auto      It = std::find_if(Running.begin(), Running.end(), [&](const Job &One) {
            return One.Pid == Pid && One.Generation == Generation && !One.Over && !One.Front;
        });
        if (It == Running.end()) return std::unexpected(-ERR_PROCESS_NOT_FOUND);
        It->Front     = true;
        It->Wants     = false;
        It->End.Asked = false;
    }
    std::uint32_t Display = 0;
    (void)display_front(Pid, Generation, &Display);
    return StandBy(Pid, Generation);
}

std::vector<BehindNow> Jobs::Living() const
{
    std::lock_guard        Held(Lock);
    std::vector<BehindNow> Now;
    for (const Job &One : Running)
        if (!One.Front && !One.Over && One.What.Living != PROGRAM_SERVES)
            Now.push_back(BehindNow{One.What.Who, One.Pid, One.Generation, One.Wants});
    return Now;
}

std::optional<std::vector<file_id_t>> Jobs::TakeGift()
{
    std::lock_guard Held(Lock);
    return std::exchange(Gift, std::nullopt);
}

void Jobs::Show(const Look &Shown)
{
    std::lock_guard Held(Lock);
    Seen = Shown;
}

}  // namespace hub
