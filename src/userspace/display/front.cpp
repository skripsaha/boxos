/*
 * front.cpp — the program in front of the person, and the ones behind. See
 * front.h and box/display.h.
 *
 * A keeper — a hub — says which program it keeps in front of the person and
 * which it lets live behind; the keeper is the asking program, by the kernel's
 * word, and only a program that wears `system` keeps others. In front is a
 * stack of who keeps whom, the newest on top: a hub that starts a hub that
 * starts edit keeps the second hub, which keeps edit, and a chord pressed is
 * told to the keeper on top, about the program it keeps there.
 *
 * Behind is a set of the programs keepers let go. With them live behind the
 * programs they start, however far down: the display hears every program
 * started, with the program it was started from (process:spawned, said before
 * the new program's first step), and a program is behind when it, or one it
 * was started from, was let go. A program behind hears no key — a strand of
 * it that asks to listen waits, and the keeper is told, once until the
 * program comes forward — and coming forward, or going behind, its whole
 * family goes with it. A record goes with the death of either side — but the
 * programs a let-go one started stay behind when it dies, kept as it was.
 */

#include "front.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "box/core/cabin.h"
#include "box/debug.h"
#include "box/error.h"
#include "box/ipc.h"
#include "box/memory.h"
#include "box/system.h"
#include "box/touch.h"

#include "lanes.h"

namespace {

// A program, by its main strand in one incarnation.
struct Program {
    std::uint32_t Pid = 0;
    std::uint32_t Generation = 0;

    bool operator==(const Program &) const = default;
};

// A program started while the display lives, and the program it was started
// from.
struct Started {
    Program Child;
    Program Parent;
};

struct Kept {
    Program Keeper;
    Program Front;
};

struct Behind {
    Program Keeper;
    Program Back;
    bool    Told = false;   // its keeper heard that it wants the keyboard
};

std::vector<Started> g_started;
std::vector<Kept>    g_front;    // the newest keeper on top, at the back
std::vector<Behind>  g_behind;
TouchTagPair         g_news;     // DISPLAY_FRONT_TAG
TouchTag             g_spawned = TOUCH_TAG_INVALID;

void Tell(const Program &Keeper, DisplayFrontWhat What, const Program &About) noexcept
{
    const DisplayFrontNews News{Keeper.Pid, Keeper.Generation, static_cast<std::uint8_t>(What), {0, 0, 0},
                                About.Pid, About.Generation};
    if (touch_send(g_news, &News, sizeof(News), 0) > 0) return;
    static bool Said = false;
    if (!Said) {
        Said = true;
        kdbg_print("[display] news for the keeper pid %u reached nobody", Keeper.Pid);
    }
}

// Only a program that wears `system` keeps others: whom another program hears
// the keyboard through is no ordinary program's to say.
bool MayKeep(const AskFrom &From) noexcept
{
    ProcMate     *Mates = nullptr;
    std::uint32_t Total = 0;
    const int     Delivered = proc_crew("system", &Mates, &Total);
    bool          Found = false;
    for (int I = 0; I < Delivered && !Found; I++)
        Found = Mates[I].pid == From.program_pid && Mates[I].generation == From.program_generation;
    free(Mates);
    return Found;
}

void Reply(const Result &Asked, std::uint8_t Command, error_t Error, bool Heard) noexcept
{
    const DisplayFrontReply Answer{Command, static_cast<std::uint8_t>(Heard ? 1 : 0), {0, 0},
                                   static_cast<std::int32_t>(Error), cabin_info()->pid};
    (void)answer(&Asked, &Answer, sizeof(Answer));
}

// The record that holds the program behind: its own, or that of a program it
// was started from, however far up; nullptr when it is not behind. A chain is
// never longer than the programs heard started, so a walk is never longer.
Behind *HeldBehind(Program Which) noexcept
{
    for (std::size_t Steps = 0; Steps <= g_started.size(); Steps++) {
        const auto Own = std::find_if(g_behind.begin(), g_behind.end(),
                                      [&](const Behind &One) { return One.Back == Which; });
        if (Own != g_behind.end()) return &*Own;
        const auto From = std::find_if(g_started.begin(), g_started.end(),
                                       [&](const Started &One) { return One.Child == Which; });
        if (From == g_started.end()) return nullptr;
        Which = From->Parent;
    }
    return nullptr;
}

// The program kept in front of the keeper — taken off its other places first,
// the room made before anything is moved, so a refusal changes nothing.
void BringForward(const Program &Keeper, const Program &Which)
{
    g_front.reserve(g_front.size() + 1);
    std::erase_if(g_behind, [&](const Behind &One) { return One.Back == Which; });
    std::erase_if(g_front, [&](const Kept &One) { return One.Front == Which; });
    g_front.push_back(Kept{Keeper, Which});
}

void SendBehind(const Program &Keeper, const Program &Which)
{
    g_behind.reserve(g_behind.size() + 1);
    std::erase_if(g_front, [&](const Kept &One) { return One.Front == Which; });
    std::erase_if(g_behind, [&](const Behind &One) { return One.Back == Which; });
    g_behind.push_back(Behind{Keeper, Which, false});
}

}  // namespace

bool FrontStart() noexcept
{
    g_news    = touch_intern(DISPLAY_FRONT_TAG);
    g_spawned = touch_pair_choose(touch_intern(TOUCH_TAG_PROCESS_SPAWNED));
    if (g_spawned == TOUCH_TAG_INVALID || touch_claim(g_spawned, TOUCH_REST, 0, 0) != 0) return false;
    if (touch_pair_choose(g_news) == TOUCH_TAG_INVALID) return false;
    // Here: every keeper says again what it keeps — this display may be one
    // raised after a fall, and knows nothing of what the one before was told.
    // Their questions wait for the loop; nobody listening is a boot's quiet.
    const DisplayFrontNews Here{0, 0, DISPLAY_FRONT_HERE, {0, 0, 0}, 0, 0};
    (void)touch_send(g_news, &Here, sizeof(Here), 0);
    return true;
}

void FrontAsked(const Result &Asked, const AskFrom &From, const DisplayFrontRequest &Request) noexcept
{
    if (!MayKeep(From)) {
        Reply(Asked, Request.command, ERR_ACCESS_DENIED, false);
        return;
    }
    const Program Keeper{From.program_pid, From.program_generation};
    const Program Which{Request.pid, Request.generation};
    if (Request.command == DISP_CMD_ASK_END) {
        Reply(Asked, Request.command, OK, LanesNotice(Which.Pid, Which.Generation, CONSOLE_NOTICE_END));
        return;
    }
    try {
        if (Request.command == DISP_CMD_FRONT) BringForward(Keeper, Which);
        else                                   SendBehind(Keeper, Which);
    } catch (...) {
        Reply(Asked, Request.command, ERR_NO_MEMORY, false);
        return;
    }
    LanesRecheck();
    Reply(Asked, Request.command, OK, false);
}

bool FrontChord(const kb_event_t &Key) noexcept
{
    const std::uint8_t Both = KB_MOD_CTRL | KB_MOD_ALT;
    if ((Key.mods & Both) != Both) return false;
    const bool Extended = (Key.mods & KB_MOD_EXTENDED) != 0;
    const bool End      = Extended ? Key.scancode == KEY_END : Key.ascii == '-';
    const bool LetGo    = Extended ? Key.scancode == KEY_HOME : Key.ascii == '=';
    if (!End && !LetGo) return false;
    // The chord is about the newest program kept in front that is not behind
    // with a family let go. With none it is nobody's: taken all the same, for
    // it is the system's and never a program's key.
    FrontSpawnStep();
    for (auto Top = g_front.rbegin(); Top != g_front.rend(); ++Top) {
        if (HeldBehind(Top->Front)) continue;
        Tell(Top->Keeper, End ? DISPLAY_FRONT_END : DISPLAY_FRONT_LET_GO, Top->Front);
        break;
    }
    return true;
}

bool FrontBehind(std::uint32_t ProgramPid, std::uint32_t ProgramGeneration) noexcept
{
    // Every start heard before the answer: a program is started — and heard
    // so — before its first step, so before any question of it.
    FrontSpawnStep();
    return HeldBehind(Program{ProgramPid, ProgramGeneration}) != nullptr;
}

void FrontWants(std::uint32_t ProgramPid, std::uint32_t ProgramGeneration) noexcept
{
    Behind *Root = HeldBehind(Program{ProgramPid, ProgramGeneration});
    if (!Root || Root->Told) return;
    Root->Told = true;
    Tell(Root->Keeper, DISPLAY_FRONT_WANTS, Root->Back);
}

void FrontDeath(std::uint32_t Pid, std::uint32_t Generation) noexcept
{
    // Its start heard first: said before its first step, so before its death.
    FrontSpawnStep();
    const Program Dead{Pid, Generation};
    std::erase_if(g_front, [&](const Kept &One) { return One.Front == Dead || One.Keeper == Dead; });

    // A program let go that dies leaves what it started behind, kept by its
    // keeper: the family stays whole, and none of it takes the keyboard from
    // the one in front. A program whose keeper died is nobody's to hold back:
    // it hears again.
    std::vector<Behind> Heirs;
    try {
        for (const Behind &One : g_behind) {
            if (One.Back != Dead || One.Keeper == Dead) continue;
            for (const Started &Child : g_started)
                if (Child.Parent == Dead) Heirs.push_back(Behind{One.Keeper, Child.Child, false});
        }
    } catch (...) {
        Heirs.clear();
        kdbg_print("[display] no room to keep behind what pid %u started; it hears again", Pid);
    }
    std::erase_if(g_behind, [&](const Behind &One) { return One.Back == Dead || One.Keeper == Dead; });
    for (const Behind &Heir : Heirs) {
        try {
            g_behind.push_back(Heir);
        } catch (...) {
            kdbg_print("[display] no room to keep pid %u behind; it hears again", Heir.Back.Pid);
        }
    }

    // What the dead started is counted as started by the one that started the
    // dead, so a family behind stays whole; with nobody above, it is nobody's.
    const auto Own = std::find_if(g_started.begin(), g_started.end(),
                                  [&](const Started &One) { return One.Child == Dead; });
    if (Own != g_started.end()) {
        const Program Above = Own->Parent;
        for (Started &One : g_started)
            if (One.Parent == Dead) One.Parent = Above;
    } else {
        std::erase_if(g_started, [&](const Started &One) { return One.Parent == Dead; });
    }
    std::erase_if(g_started, [&](const Started &One) { return One.Child == Dead; });
    LanesRecheck();
}

bool FrontSpawnStep() noexcept
{
    bool  Heard = false;
    Touch Event;
    while (g_spawned != TOUCH_TAG_INVALID && touch_try_pop_tag(g_spawned, &Event)) {
        Heard = true;
        if (!(Event.flags & TOUCH_FLAG_KERNEL) || Event.payload_len < sizeof(TouchProcessSpawned)) continue;
        TouchProcessSpawned Spawned;
        std::memcpy(&Spawned, Event.payload, sizeof(Spawned));
        try {
            g_started.push_back(Started{Program{Spawned.pid, Spawned.generation},
                                        Program{Spawned.parent_program_pid, Spawned.parent_program_generation}});
        } catch (...) {
            kdbg_print("[display] no room to know who started pid %u; it hears as one started by nobody",
                       Spawned.pid);
        }
    }
    return Heard;
}
