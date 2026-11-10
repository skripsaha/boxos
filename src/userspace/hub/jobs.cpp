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

#include "context.h"
#include "news.h"
#include "programs.h"
#include "say.h"
#include "tags.h"
#include "windows.h"

namespace hub {

namespace {

// The tags a hub's two strands wake each other on — the keeping strand on
// keeper:wake, the prompt's on keeper:settled — the payload the pid of the hub
// it is for. Two, for a Touch tag is claimed once by a program and heard by
// the strand that claimed it first. Occurrences the kernel's muster books, so
// they stand on any volume and take none of a volume's names.
constexpr const char *WakeTagName    = "keeper:wake";
constexpr const char *SettledTagName = "keeper:settled";

// What of what a program gave back is kept as `them`: the file its screen
// went into, or its Souvenir when it was asked for and could be read off.
std::optional<std::vector<file_id_t>> KeptOf(const Start &What, bool Collected, std::vector<file_id_t> Given)
{
    if (What.Screen != 0) return std::vector<file_id_t>{What.Screen};
    if (What.Souvenir && Collected) return Given;
    return std::nullopt;
}

// The name a running program goes by: its file's stem, which it wears among
// its tags — the first of them that names a program on the volume. Looked for
// among those a hub starts, that wear app or utility.
std::string NameOf(const Which &Program)
{
    std::string Tags;
    for (const char *Kind : {"app", "utility"}) {
        ProcMate     *Mates = nullptr;
        std::uint32_t Total = 0;
        const int     Delivered = proc_crew(Kind, &Mates, &Total);
        for (int I = 0; I < Delivered && Tags.empty(); I++)
            if (Mates[I].pid == Program.Pid && Mates[I].generation == Program.Generation && Mates[I].tags)
                Tags = Mates[I].tags;
        free(Mates);
        if (!Tags.empty()) break;
    }
    std::vector<std::string> Stems;
    programs::EachCommand([&](file_id_t, std::string_view FileName) {
        Stems.emplace_back(FileName.substr(0, programs::StemLength(FileName)));
    });
    for (std::string_view Tag : tags::Split(Tags))
        if (std::find(Stems.begin(), Stems.end(), Tag) != Stems.end()) return std::string(Tag);
    return "a program";
}

}  // namespace

bool Jobs::Begin(const Look &Shown)
{
    Seen       = Shown;
    PageStale  = true;
    KeysStale  = true;
    WakeTag    = touch_intern(WakeTagName);
    SettledTag = touch_intern(SettledTagName);
    Woken      = touch_pair_choose(WakeTag);
    Settling   = touch_pair_choose(SettledTag);
    // The prompt's strand hears keeper:settled before the keeping strand can say it.
    if (Settling == TOUCH_TAG_INVALID || touch_claim(Settling, TOUCH_REST, 0, 0) != 0) {
        Say("hub: keeper:settled cannot be heard; nothing can be started\n");
        return false;
    }
    bool Ready = false, Heard = false;
    try {
        Keeper = std::thread([this, &Ready, &Heard] {
            // Claimed on the keeping strand itself, before the first start:
            // no death can come before its ear.
            Deaths = touch_pair_choose(touch_intern(TOUCH_TAG_PROCESS_DIED));
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
            SaySettled();
            if (Claimed) Keep();
        });
    } catch (...) {
        Say("hub: no strand could be made to keep its programs; nothing can be started\n");
        return false;
    }
    Await([&] { return Ready; });
    if (!Heard) Say("hub: the ends of programs cannot be heard; nothing can be started\n");
    return Heard;
}

Jobs::~Jobs()
{
    if (!Keeper.joinable()) return;
    Stopping.store(true);
    Wake();
    Keeper.join();
}

// Under the lock: the program the prompt stands by for, not yet collected.
Jobs::Job *Jobs::TaskOf(const Which &Program)
{
    const auto It = std::find_if(Tasks.begin(), Tasks.end(), [&](const Job &One) { return One.Program == Program; });
    return It == Tasks.end() ? nullptr : &*It;
}

// Who lives behind changed, or how the person set it shown: the line at the
// top of the screen is drawn again by the keeping strand — a layer is the
// strand's that opened it, so one strand alone ever draws it.
void Jobs::BehindChanged()
{
    {
        std::lock_guard Held(Lock);
        BehindStale = true;
    }
    Wake();
}

bool Jobs::DrawBehind()
{
    Look                   Shown;
    std::vector<BehindOne> Now;
    std::optional<Which>   On;
    {
        std::lock_guard Held(Lock);
        if (!BehindStale) return false;
        BehindStale = false;
        Shown       = Seen;
        Now         = Away.OnCircle();
        On          = Screen;
    }
    windows::ShowBehind(Shown, Now, On);
    return true;
}

void Jobs::Wake()
{
    const std::uint32_t Me = cabin_info()->pid;
    (void)touch_send(WakeTag, &Me, sizeof(Me), 0);
}

void Jobs::SaySettled()
{
    const std::uint32_t Me = cabin_info()->pid;
    (void)touch_send(SettledTag, &Me, sizeof(Me), 0);
}

// ---- the keeping strand ------------------------------------------------------

// Everything it waits on looked at, and asleep when nothing came: the mark
// taken first, so whatever comes after it wakes the sleep. A start is looked
// at before the deaths, so the death of a program just started finds it kept.
void Jobs::Keep()
{
    while (!Stopping.load()) {
        const TurnInMark Mark  = box_mark();
        bool             Moved = HearWake();
        Moved |= StartAsked();
        Moved |= HearDeaths();
        Moved |= HearDisplay();
        Moved |= TellKeys();
        Moved |= TellPage();
        Moved |= DrawBehind();
        if (!Moved && !Stopping.load()) box_turn_in(Mark);
    }
    // The line of who lives behind is this strand's layer: taken down by it.
    windows::ShowBehind(Look{}, {}, std::nullopt);
}

bool Jobs::HearWake()
{
    bool  Heard = false;
    Touch Event{};
    while (touch_try_pop_tag(Woken, &Event)) Heard = true;
    return Heard;
}

// The next program the prompt asked for, started — the keeping strand's
// child — and kept by the display on the screen, the one from behind that was
// there going behind again; or one that lives on by itself or serves, behind.
// A machine with no display has nobody to keep it from, and nothing is lost.
bool Jobs::StartAsked()
{
    Asked *Next = nullptr;
    {
        std::lock_guard Held(Lock);
        if (Starts.empty()) return false;
        Next = Starts.front();
        Starts.pop_front();
    }
    const std::string   Context = Next->OnScreen ? std::string() : context::Whole().value_or(std::string());
    std::uint32_t       Generation = 0;
    const int           Pid = proc_exec_file(Next->What.Program, Next->What.Luggage.data(),
                                             Next->What.Luggage.size(), nullptr, &Generation);
    const Which         Started{Pid > 0 ? static_cast<std::uint32_t>(Pid) : 0, Generation};
    std::optional<Which> Displaced;
    if (Pid > 0) {
        std::lock_guard Held(Lock);
        if (Next->OnScreen) {
            Job One;
            One.What = Next->What;
            One.What.Luggage.clear();
            One.Program = Started;
            Tasks.push_back(std::move(One));
            Displaced = std::exchange(Screen, Started);
            Away.Forget();
        } else {
            Away.Add(BehindOne{Next->What.Who, Started, Context, 0, 0, Next->What.Living == PROGRAM_SERVES});
        }
    }
    if (Pid > 0) {
        std::uint32_t Display = 0;
        if (Displaced) Kept(display_back(Displaced->Pid, Displaced->Generation, &Display), Display);
        Kept(Next->OnScreen ? display_front(Started.Pid, Started.Generation, &Display)
                            : display_back(Started.Pid, Started.Generation, &Display),
             Display);
        BehindChanged();
    }
    {
        std::lock_guard Held(Lock);
        Next->Rc      = Pid > 0 ? 0 : (Pid == 0 ? -ERR_SPAWN_FAILED : Pid);
        Next->Program = Started;
        Next->Done    = true;
    }
    SaySettled();
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
// hub's programs. One behind is gone from the circle, and nothing is said:
// it lived on its own. One the prompt stands by for has its end and what it
// gave read, and the prompt is told.
void Jobs::Died(const TouchProcessDied &Death)
{
    const Which Dead{Death.pid, Death.generation};
    bool        Changed = false, Settle = false;
    {
        std::lock_guard Held(Lock);
        Changed = Away.Remove(Dead);
        if (Screen == Dead) {
            Screen.reset();
            Settle = true;
        }
        Job *One = TaskOf(Dead);
        if (One && !One->Away) {
            One->Over     = true;
            One->End.Seen = true;
            One->End.Code = Death.exit_code;
            One->End.Fell = Death.fell != 0;
            if (One->End.Fell) {
                One->End.Vector   = Death.vector;
                const char *Named = std::find(Death.in, Death.in + sizeof(Death.in), '\0');
                One->End.In.assign(Death.in, static_cast<std::size_t>(Named - Death.in));
            }
            // What it queued is read off whether it is kept or not, so no
            // stale gift is met later. It came to this strand, the one that
            // started it.
            file_id_t *Ids       = nullptr;
            size_t     Count     = 0;
            const bool Collected = souvenir_collect(Dead.Pid, &Ids, &Count) == 0;
            One->End.Kept        = KeptOf(One->What, Collected,
                                          Collected && Ids ? std::vector<file_id_t>(Ids, Ids + Count)
                                                           : std::vector<file_id_t>());
            free(Ids);
            Settle = true;
        }
    }
    if (Settle) SaySettled();
    if (Changed) BehindChanged();
}

// The display's answer to a question about what the hub keeps: who the
// display is, for the news it sends. Display is read here, by reference, once
// the question is answered — whichever of the two arguments was made first.
void Jobs::Kept(int Rc, const std::uint32_t &Display)
{
    if (Rc != OK) return;
    std::lock_guard Held(Lock);
    DisplayPid = Display;
}

// A display came — raised again after a fall, it knows nothing of what the
// hub told the one before: what is on the screen and every one behind said
// again, and the hub's keys and the page the person chose; who the display is
// learnt from its answers.
void Jobs::Rekeep()
{
    std::optional<Which> On;
    std::vector<Which>   Back;
    {
        std::lock_guard Held(Lock);
        PageStale = true;
        KeysStale = true;
        On        = Screen;
        for (const BehindOne &One : Away.All())
            if (One.Program != On) Back.push_back(One.Program);
    }
    std::uint32_t Display = 0;
    for (const Which &One : Back) Kept(display_back(One.Pid, One.Generation, &Display), Display);
    if (On) Kept(display_front(On->Pid, On->Generation, &Display), Display);
}

bool Jobs::HearDisplay()
{
    bool  Heard = false;
    Touch Event{};
    while (News != TOUCH_TAG_INVALID && touch_try_pop_tag(News, &Event)) {
        Heard = true;
        DisplayFrontNews Said;
        if (Event.payload_len < sizeof(Said)) continue;
        std::memcpy(&Said, Event.payload, sizeof(Said));
        if (Said.what == DISPLAY_FRONT_HERE) {
            Rekeep();
            continue;
        }
        std::uint32_t Display = 0;
        {
            std::lock_guard Held(Lock);
            Display = DisplayPid;
        }
        if (Display == 0 || Event.source_pid != Display) continue;
        if (Said.keeper_pid != cabin_info()->pid || Said.keeper_generation != cabin_info()->generation) continue;
        const Which About{Said.pid, Said.generation};
        if (Said.what == DISPLAY_FRONT_KEY) {
            Pressed(Said.key, About);
            continue;
        }
        if (Said.what == DISPLAY_FRONT_GIVEN) {
            Given(About);
            continue;
        }
        if (Said.what != DISPLAY_FRONT_WANTS) continue;
        std::string Who;
        Look        Shown;
        {
            std::lock_guard Held(Lock);
            if (!Away.Asks(About)) continue;
            Who   = Away.Find(About)->Who;
            Shown = Seen;
        }
        news::Asks(Shown, Who);
        BehindChanged();
    }
    return Heard;
}

// One behind that a hub which ended kept, given to this one by the display:
// on this hub's circle from here, and said.
void Jobs::Given(const Which &Program)
{
    const std::string Who     = NameOf(Program);
    const std::string Context = context::Whole().value_or(std::string());
    Look              Shown;
    {
        std::lock_guard Held(Lock);
        if (Away.Find(Program)) return;
        Away.Add(BehindOne{Who, Program, Context, 0, 0, false});
        Shown = Seen;
    }
    news::Given(Shown, Who);
    BehindChanged();
}

// ---- the keys ------------------------------------------------------------------

// One of the hub's keys, pressed over About — the program on the screen as
// the display saw it, none when the hub's own prompt was. Stop and let-go act
// on the program only while it is the one on the screen still.
void Jobs::Pressed(std::uint8_t Key, const Which &About)
{
    if (Key == KeyStop) Stop(About);
    else if (Key == KeyLetGo) LetGo(About);
    else if (Key == KeyNext) Next();
}

// Asked first: a program that hears it ends as it chooses — edit asks what
// to do with what is not saved. One none of whose strands reads, or one asked
// already, ends at once.
void Jobs::Stop(const Which &About)
{
    bool Again = false;
    {
        std::lock_guard Held(Lock);
        if (Screen != About) return;
        if (Job *One = TaskOf(About)) {
            Again          = One->End.Asked;
            One->End.Asked = true;
        } else if (BehindOne *One = Away.Find(About)) {
            Again           = One->AskedToEnd;
            One->AskedToEnd = true;
        } else {
            return;
        }
    }
    if (Again || display_ask_end(About.Pid, About.Generation) != 1) (void)proc_finish(About.Pid, About.Generation);
}

// Under the lock: the program the prompt stands by for gone behind — on the
// circle from here, the prompt told; quietly when another takes the screen.
// False when it is not one to go: ended, or behind already.
bool Jobs::SendBehind(Job &One, bool Quietly, const std::string &Context)
{
    if (One.Over || One.Away) return false;
    One.Away    = true;
    One.Quietly = Quietly;
    Away.Add(BehindOne{One.What.Who, One.Program, Context, 0, 0, false});
    return true;
}

void Jobs::LetGo(const Which &About)
{
    const std::string Context = context::Whole().value_or(std::string());
    {
        std::lock_guard Held(Lock);
        if (Screen != About) return;
        if (Job *One = TaskOf(About)) SendBehind(*One, false, Context);
        Screen.reset();
        Away.Forget();
    }
    std::uint32_t Display = 0;
    Kept(display_back(About.Pid, About.Generation, &Display), Display);
    SaySettled();
    BehindChanged();
}

// The next place on the circle: the program on the screen goes behind — one
// the prompt stood by for joins the circle as it goes — and the next one
// comes, or the hub's own prompt after the last.
void Jobs::Next()
{
    const std::string    Context = context::Whole().value_or(std::string());
    std::optional<Which> Back;
    std::optional<BehindOne> Coming;
    {
        std::lock_guard Held(Lock);
        Back = Screen;
        if (Back)
            if (Job *One = TaskOf(*Back)) SendBehind(*One, true, Context);
        Coming = Away.Next(Back, Seen, Context);
        if (!Back && !Coming) return;
        Screen.reset();
        if (Coming) Screen = Coming->Program;
    }
    std::uint32_t Display = 0;
    if (Back) Kept(display_back(Back->Pid, Back->Generation, &Display), Display);
    if (Coming) Kept(display_front(Coming->Program.Pid, Coming->Program.Generation, &Display), Display);
    SaySettled();
    BehindChanged();
}

// ---- what the prompt's strand asks -------------------------------------------

// The prompt's strand waits until Done — looked at under the lock — says so,
// woken on keeper:settled after every change the keeping strand makes.
// Looked at before each wait, so a change between the look and the wait is
// already a Touch in its ring.
template <typename Ready>
void Jobs::Await(Ready &&Done)
{
    for (;;) {
        {
            std::lock_guard Held(Lock);
            if (Done()) return;
        }
        Touch Event{};
        (void)touch_await(Settling, &Event, 0);
    }
}

// A start handed to the keeping strand, and waited for until it is done.
std::expected<void, int> Jobs::Ask(Asked &One)
{
    {
        std::lock_guard Held(Lock);
        Starts.push_back(&One);
    }
    Wake();
    Await([&] { return One.Done; });
    if (One.Rc != 0) return std::unexpected(One.Rc);
    return {};
}

// Gone behind comes first: a program that went behind and ended before the
// prompt looked lived on its own, and its end is not said.
std::expected<Settled, int> Jobs::StandBy(const Which &Program)
{
    Await([&] {
        const Job *One = TaskOf(Program);
        return !One || One->Over || One->Away;
    });
    std::lock_guard Held(Lock);
    const auto      It = std::find_if(Tasks.begin(), Tasks.end(), [&](const Job &One) { return One.Program == Program; });
    if (It == Tasks.end()) return std::unexpected(-ERR_PROCESS_NOT_FOUND);
    Settled Done;
    if (It->Away) {
        Done.Behind  = true;
        Done.Quietly = It->Quietly;
    } else {
        Done.End = std::move(It->End);
    }
    Tasks.erase(It);
    return Done;
}

std::expected<Settled, int> Jobs::OnScreen(Start What)
{
    Asked One;
    One.What     = std::move(What);
    One.OnScreen = true;
    if (std::expected<void, int> Started = Ask(One); !Started) return std::unexpected(Started.error());
    return StandBy(One.Program);
}

std::expected<void, int> Jobs::Behind(Start What)
{
    Asked One;
    One.What = std::move(What);
    return Ask(One);
}

void Jobs::AwaitScreen()
{
    Await([&] { return !Screen; });
}

std::vector<BehindOne> Jobs::Living() const
{
    std::lock_guard Held(Lock);
    return Away.OnCircle();
}

void Jobs::Show(const Look &Shown)
{
    {
        std::lock_guard Held(Lock);
        PageStale = PageStale || Shown.Text != Seen.Text || Shown.Page != Seen.Page;
        for (std::uint8_t Key = 0; Key < KeyCount; Key++)
            KeysStale = KeysStale || !KeyChordSame(&Shown.Keys[Key], &Seen.Keys[Key]);
        if (Shown.Order != Seen.Order || Shown.BehindContextFirst != Seen.BehindContextFirst) Away.Forget();
        Seen = Shown;
    }
    BehindChanged();
}

// The screen's colours the person chose, told to the display when it has not
// heard them: it draws in them every colour no program chose. With no display
// to hear, the next one that comes is told.
bool Jobs::TellPage()
{
    Color Text = 0, Page = 0;
    {
        std::lock_guard Held(Lock);
        if (!PageStale) return false;
        PageStale = false;
        Text      = Seen.Text;
        Page      = Seen.Page;
    }
    std::uint32_t Display = 0;
    Kept(display_page(Text, Page, &Display), Display);
    return true;
}

// The hub's keys, told to the display when it has not heard them: it takes
// them before any program, and tells the hub which was pressed.
bool Jobs::TellKeys()
{
    KeyChord Keys[KeyCount];
    {
        std::lock_guard Held(Lock);
        if (!KeysStale) return false;
        KeysStale = false;
        std::copy(std::begin(Seen.Keys), std::end(Seen.Keys), Keys);
    }
    std::uint32_t Display = 0;
    Kept(display_keys(Keys, KeyCount, KeyNext, &Display), Display);
    return true;
}

}  // namespace hub
