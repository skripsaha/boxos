/* jobs.h — the programs this hub starts: the one on the screen, stood by
 * until it ends or goes behind, and the ones living on their own behind the
 * hub (circle.h). What is said of them is news.h's.
 *
 * One strand of the hub keeps them all, the keeping strand. It starts every
 * program, so each is its child and gives back to it what it gives; it hears
 * every death (process:died, claimed before the first start, so no end is
 * missed) and the display's news (box/display.h): the keys the hub said it
 * acts on, pressed over whatever is on the screen —
 *
 *   stop     the program on the screen asked to finish, ended when pressed
 *            again or when no strand of it reads
 *   let-go   the program on the screen goes behind and lives on its own: the
 *            hub neither waits for it nor speaks of its end; the screen is
 *            the hub's again
 *   next     those behind come to the screen one by one, the hub's own prompt
 *            after the last
 *
 * — a program behind asking for the keyboard, which is said, and one behind
 * that a hub which ended kept, given to this one, which joins the circle
 * (box/display.h DISPLAY_FRONT_GIVEN). It tells
 * the display the hub's keys and the screen's colours the person chose, at
 * the start, when they change, and to a display that comes. The prompt's
 * strand only asks — start this — and stands by while a program is on the
 * screen. Each wakes the other on a tag of its own (keeper:wake,
 * keeper:settled): a strand that waits on its Touch keeps every other event
 * that comes for it — the history's growth, a settings file saved — in its
 * stash for its reader, where a strand asleep on anything else would leave
 * them unread. */
#ifndef HUB_JOBS_H
#define HUB_JOBS_H

#include <atomic>
#include <cstdint>
#include <deque>
#include <expected>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "box/life.h"
#include "box/touch.h"
#include "box/types.h"

#include "circle.h"
#include "look.h"

namespace hub {

// How a started program ended, and what it gave back.
struct Ending {
    bool                                  Seen = false;    // its end was seen: Code is its own
    std::int32_t                          Code = 0;        // proc_exit.h: its exit, or why it was ended
    bool                                  Fell = false;    // the kernel said how it fell: Vector, In
    std::uint8_t                          Vector = 0;      // the exception it fell on
    std::string                           In;              // the function it fell in; "" unnamed
    bool                                  Asked = false;   // the person asked it to finish
    std::optional<std::vector<file_id_t>> Kept;            // what it gave back that is kept as `them`
};

// A program to start, how it lives, and what of what it gives back is kept.
struct Start {
    file_id_t     Program = 0;
    std::string   Who;                    // the name the person used
    std::string   Luggage;                // its Form, filled
    ProgramLiving Living = PROGRAM_TASK;  // box/life.h
    file_id_t     Screen = 0;             // the file its screen goes into, kept when it ends; 0 none
    bool          Souvenir = false;       // its Souvenir is asked for, and kept when it ends
};

// How a program the prompt stood by for settled: it ended, or it went behind
// and End says nothing — quietly when another came to the screen in its
// place, so nothing is said of it.
struct Settled {
    bool   Behind = false;
    bool   Quietly = false;
    Ending End;
};

class Jobs {
public:
    Jobs() = default;
    Jobs(const Jobs &)            = delete;
    Jobs &operator=(const Jobs &) = delete;
    // The keeping strand stopped and waited for: it uses what the hub holds,
    // and goes before it.
    ~Jobs();

    // The keeping strand started, with the hub set as Shown. False: no
    // program can be started here, and why was said.
    bool Begin(const Look &Shown);

    // A program started on the screen, stood by until it ends or goes behind.
    // Not started: the kernel's refusal, a -error_t.
    std::expected<Settled, int> OnScreen(Start What);
    // A program that lives on by itself or serves, started behind. Not
    // started: a -error_t.
    std::expected<void, int> Behind(Start What);
    // Stood by while one from behind is on the screen: back when the hub's
    // own prompt is.
    void AwaitScreen();

    // Those on the circle, and whether one of them asks for the keyboard.
    std::vector<BehindOne> Living() const;

    // How the person set the hub, for what the keeping strand says and does.
    void Show(const Look &Shown);

private:
    // A program the prompt stands by for.
    struct Job {
        Start  What;
        Which  Program;
        bool   Over = false;      // its end was heard
        bool   Away = false;      // it went behind: the prompt is told
        bool   Quietly = false;   // another came to the screen in its place
        Ending End;
    };
    struct Asked {
        Start         What;
        bool          OnScreen = false;
        bool          Done = false;
        int           Rc = 0;
        Which         Program;
    };

    void Keep();
    void BehindChanged();
    bool DrawBehind();
    void Wake();
    void SaySettled();
    bool StartAsked();
    bool HearDeaths();
    bool HearDisplay();
    bool HearWake();
    bool TellPage();
    bool TellKeys();
    void Died(const TouchProcessDied &Death);
    void Given(const Which &Program);
    void Pressed(std::uint8_t Key, const Which &About);
    void Stop(const Which &About);
    void LetGo(const Which &About);
    void Next();
    bool SendBehind(Job &One, bool Quietly, const std::string &Context);
    void Rekeep();
    void Kept(int Rc, const std::uint32_t &Display);
    std::expected<void, int> Ask(Asked &One);
    template <typename Ready> void Await(Ready &&Done);
    std::expected<Settled, int> StandBy(const Which &Program);
    Job *TaskOf(const Which &Program);

    std::thread                           Keeper;
    std::atomic<bool>                     Stopping{false};
    Look                                  Seen{};   // the keeping strand's own copy
    mutable std::mutex                    Lock;     // all of the below
    std::vector<Job>                      Tasks;    // those the prompt stands by
    Circle                                Away;
    std::optional<Which>                  Screen;   // the program on the screen; none: the hub's prompt
    std::deque<Asked *>                   Starts;
    bool                                  BehindStale = false;   // the line of who lives behind is old
    bool                                  PageStale = false;     // the display has not been told Seen's page
    bool                                  KeysStale = false;     // nor Seen's keys
    TouchTag                              Deaths = TOUCH_TAG_INVALID;
    TouchTag                              News = TOUCH_TAG_INVALID;
    TouchTag                              Woken = TOUCH_TAG_INVALID;      // the keeping strand's ear
    TouchTag                              Settling = TOUCH_TAG_INVALID;   // the prompt's strand's
    TouchTagPair                          WakeTag{};
    TouchTagPair                          SettledTag{};
    std::uint32_t                         DisplayPid = 0;   // who says the display's news
};

}  // namespace hub

#endif
