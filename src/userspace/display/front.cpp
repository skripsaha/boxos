/*
 * front.cpp — the program in front of the person, and the ones behind. See
 * front.h and box/display.h.
 *
 * A keeper — a hub — says which program it keeps in front of the person and
 * which it lets live behind; the keeper is the asking program, by the kernel's
 * word, and only a program that wears `system` keeps others. In front is a
 * stack of who keeps whom, the newest on top: a hub that starts a hub that
 * starts edit keeps the second hub, which keeps edit. The keys a keeper acts
 * on are its own word, kept in a stack too, the newest on top; one pressed is
 * told to the keeper whose screen is shown — the keeper on top of who keeps
 * whom, about the program it keeps there, or that program itself when it is
 * a keeper with nothing of its own in front. A key the keeper said is about
 * the program on the screen goes to whoever keeps that program even then: a
 * hub on the screen is stopped, or let go, by the hub that started it.
 *
 * A keeper that ends leaves what it kept behind behind: given to the keeper
 * that kept it, or, with none, to the next keeper that says its keys — so a
 * program let go to live on its own outlives the hub that let it go, even
 * one that fell, and comes back on the screen through the one that follows.
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
#include <optional>
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

// The keys a keeper said it acts on, in the order it told them; the first
// OnScreen about the program on the screen.
struct Keys {
    Program               Keeper;
    std::vector<KeyChord> Chords;
    std::uint32_t         OnScreen = 0;
};

std::vector<Started> g_started;
std::vector<Kept>    g_front;    // the newest keeper on top, at the back
std::vector<Behind>  g_behind;
std::vector<Keys>    g_keys;     // the newest word on top, at the back
TouchTagPair         g_news;     // DISPLAY_FRONT_TAG
TouchTag             g_spawned = TOUCH_TAG_INVALID;

void Tell(const Program &Keeper, DisplayFrontWhat What, const Program &About, std::uint8_t Key = 0) noexcept
{
    const DisplayFrontNews News{Keeper.Pid, Keeper.Generation, static_cast<std::uint8_t>(What), Key, {0, 0},
                                About.Pid, About.Generation};
    if (touch_send(g_news, &News, sizeof(News), 0) > 0) return;
    static bool Said = false;
    if (!Said) {
        Said = true;
        kdbg_print("[display] news for the keeper pid %u reached nobody", Keeper.Pid);
    }
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

// Only a program that wears `system` keeps others: whom another program hears
// the keyboard through, and the screen's colours, are no ordinary program's to
// say.
bool FrontKeeper(const AskFrom &From) noexcept
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

bool FrontStart() noexcept
{
    g_news    = touch_intern(DISPLAY_FRONT_TAG);
    g_spawned = touch_pair_choose(touch_intern(TOUCH_TAG_PROCESS_SPAWNED));
    if (g_spawned == TOUCH_TAG_INVALID || touch_claim(g_spawned, TOUCH_REST, 0, 0) != 0) return false;
    if (touch_pair_choose(g_news) == TOUCH_TAG_INVALID) return false;
    // Here: every keeper says again what it keeps — this display may be one
    // raised after a fall, and knows nothing of what the one before was told.
    // Their questions wait for the loop; nobody listening is a boot's quiet.
    const DisplayFrontNews Here{0, 0, DISPLAY_FRONT_HERE, 0, {0, 0}, 0, 0};
    (void)touch_send(g_news, &Here, sizeof(Here), 0);
    return true;
}

void FrontAsked(const Result &Asked, const AskFrom &From, const DisplayFrontRequest &Request) noexcept
{
    if (!FrontKeeper(From)) {
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

void FrontKeysAsked(const Result &Asked, const AskFrom &From, const KeyChord *Chords, std::uint32_t Count,
                    std::uint32_t OnScreen) noexcept
{
    if (!FrontKeeper(From)) {
        Reply(Asked, DISP_CMD_KEYS, ERR_ACCESS_DENIED, false);
        return;
    }
    if (OnScreen > Count) {
        Reply(Asked, DISP_CMD_KEYS, ERR_INVALID_ARGUMENT, false);
        return;
    }
    // A key alone, or with shift alone, is typing: never taken from everybody.
    for (std::uint32_t I = 0; I < Count; I++) {
        const KeyChord &One = Chords[I];
        if (!(One.Mods & (KB_MOD_CTRL | KB_MOD_ALT)) || (One.Mods & ~KEY_CHORD_MODS) || One.Extended > 1) {
            Reply(Asked, DISP_CMD_KEYS, ERR_INVALID_ARGUMENT, false);
            return;
        }
    }
    const Program Keeper{From.program_pid, From.program_generation};
    try {
        Keys Said{Keeper, std::vector<KeyChord>(Chords, Chords + Count), OnScreen};
        g_keys.reserve(g_keys.size() + 1);
        std::erase_if(g_keys, [&](const Keys &One) { return One.Keeper == Keeper; });
        if (Count > 0) g_keys.push_back(std::move(Said));
    } catch (...) {
        Reply(Asked, DISP_CMD_KEYS, ERR_NO_MEMORY, false);
        return;
    }
    Reply(Asked, DISP_CMD_KEYS, OK, false);
    // What lives behind kept by nobody — its keeper ended with none above
    // it — is this keeper's from here.
    for (Behind &One : g_behind) {
        if (One.Keeper != Program{}) continue;
        One.Keeper = Keeper;
        Tell(Keeper, DISPLAY_FRONT_GIVEN, One.Back);
    }
}

void FrontPlacesAsked(const Result &Asked) noexcept
{
    FrontSpawnStep();
    try {
        std::vector<DisplayPlace> Places;
        const auto Mark = [&](const Program &Who, std::uint8_t Place) {
            for (DisplayPlace &One : Places)
                if (One.pid == Who.Pid && One.generation == Who.Generation) {
                    One.place = static_cast<std::uint8_t>(One.place | Place);
                    return;
                }
            Places.push_back(DisplayPlace{Who.Pid, Who.Generation, Place, {0, 0, 0}});
        };
        for (const Behind &One : g_behind) Mark(One.Back, DISPLAY_PLACE_BEHIND);
        for (const Keys &One : g_keys) Mark(One.Keeper, DISPLAY_PLACE_KEEPER);
        // Whose screen is shown: what is in front on top of who keeps whom,
        // or, with nothing in front, the newest keeper not behind.
        std::optional<Program> Shown;
        for (auto Top = g_front.rbegin(); Top != g_front.rend() && !Shown; ++Top)
            if (!HeldBehind(Top->Front)) Shown = Top->Front;
        for (auto Said = g_keys.rbegin(); Said != g_keys.rend() && !Shown; ++Said)
            if (!HeldBehind(Said->Keeper)) Shown = Said->Keeper;
        if (Shown) Mark(*Shown, DISPLAY_PLACE_SHOWN);
        if (Places.size() > 65535u) Places.resize(65535u);

        const DisplayPlacesReply Head{DISP_CMD_PLACES, 0, static_cast<std::uint16_t>(Places.size())};
        std::vector<std::uint8_t> Reply(sizeof(Head) + Places.size() * sizeof(DisplayPlace));
        std::memcpy(Reply.data(), &Head, sizeof(Head));
        if (!Places.empty()) std::memcpy(Reply.data() + sizeof(Head), Places.data(), Places.size() * sizeof(DisplayPlace));
        (void)answer(&Asked, Reply.data(), static_cast<std::uint32_t>(Reply.size()));
    } catch (...) {
        (void)answer(&Asked, nullptr, 0);
    }
}

bool FrontChord(const kb_event_t &Key) noexcept
{
    if (g_keys.empty()) return false;
    const std::vector<KeyChord> &Chords = g_keys.back().Chords;
    std::size_t                  Place  = 0;
    while (Place < Chords.size() && !KeyChordMatches(&Chords[Place], &Key)) Place++;
    if (Place == Chords.size()) return false;
    const auto Which    = static_cast<std::uint8_t>(Place);
    const bool OnScreen = Place < g_keys.back().OnScreen;
    const auto Keeps    = [](const Program &One) {
        return std::any_of(g_keys.begin(), g_keys.end(), [&](const Keys &Said) { return Said.Keeper == One; });
    };
    // A key about the program on the screen: to whoever keeps it, keeper or
    // not. Any other: to the keeper whose screen is shown — on top of who
    // keeps whom, unless what it keeps there keeps others itself, then that
    // one, its own screen.
    FrontSpawnStep();
    for (auto Top = g_front.rbegin(); Top != g_front.rend(); ++Top) {
        if (HeldBehind(Top->Front)) continue;
        if (!OnScreen && Keeps(Top->Front)) Tell(Top->Front, DISPLAY_FRONT_KEY, Program{}, Which);
        else Tell(Top->Keeper, DISPLAY_FRONT_KEY, Top->Front, Which);
        return true;
    }
    for (auto Said = g_keys.rbegin(); Said != g_keys.rend(); ++Said) {
        if (HeldBehind(Said->Keeper)) continue;
        Tell(Said->Keeper, DISPLAY_FRONT_KEY, Program{}, Which);
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
    if (!Root || Root->Told || Root->Keeper == Program{}) return;
    Root->Told = true;
    Tell(Root->Keeper, DISPLAY_FRONT_WANTS, Root->Back);
}

void FrontDeath(std::uint32_t Pid, std::uint32_t Generation) noexcept
{
    // Its start heard first: said before its first step, so before its death.
    FrontSpawnStep();
    const Program Dead{Pid, Generation};
    // Who kept the dead, in front or behind: none when nobody did.
    Program Above;
    for (const Kept &One : g_front)
        if (One.Front == Dead) Above = One.Keeper;
    for (const Behind &One : g_behind)
        if (One.Back == Dead) Above = One.Keeper;
    std::erase_if(g_front, [&](const Kept &One) { return One.Front == Dead || One.Keeper == Dead; });

    // What the dead kept behind stays behind, given to the one that kept the
    // dead, or kept by nobody until the next keeper says its keys.
    for (Behind &One : g_behind) {
        if (One.Keeper != Dead || One.Back == Dead) continue;
        One.Keeper = Above;
        One.Told   = false;
        if (Above != Program{}) Tell(Above, DISPLAY_FRONT_GIVEN, One.Back);
    }

    // A program let go that dies leaves what it started behind, kept by its
    // keeper: the family stays whole, and none of it takes the keyboard from
    // the one in front.
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
    std::erase_if(g_behind, [&](const Behind &One) { return One.Back == Dead; });
    std::erase_if(g_keys, [&](const Keys &One) { return One.Keeper == Dead; });
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
