/* jobs.h — the programs this hub starts: the one in front of the person,
 * stood by until it ends or is let go, and the ones living behind. What is
 * said of them is news.h's.
 *
 * One strand of the hub keeps them all, the keeping strand. It starts every
 * program, so each is its child and gives back to it what it gives; it hears
 * every death (process:died, claimed before the first start, so no end is
 * missed) and the display's news (box/display.h): Ctrl+Alt+End over the
 * program in front asks it to finish — and ends it at once when no strand of
 * it reads, or when it was asked already — Ctrl+Alt+Home lets it go to live
 * behind and gives the person back the prompt, and a program behind asking
 * for the keyboard is said. What becomes of a program behind is said when it
 * happens, and the display keeps the line being typed under it. The prompt's
 * strand only asks — start this, bring that forward — waking the keeping
 * strand on hub:wake, and stands by while a program is in front. */
#ifndef HUB_JOBS_H
#define HUB_JOBS_H

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "box/life.h"
#include "box/touch.h"
#include "box/types.h"

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

// How a program the prompt stood by for settled: it ended, or it was let go
// to live behind and End says nothing yet.
struct Settled {
    bool   LetGo = false;
    Ending End;
};

// A program living behind, as `front` lists it.
struct BehindNow {
    std::string   Who;
    std::uint32_t Pid = 0;
    std::uint32_t Generation = 0;
    bool          Wants = false;   // it asked for the keyboard
};

class Jobs {
public:
    // The keeping strand started, with the hub set as Shown. False: no
    // program can be started here, and why was said.
    bool Begin(const Look &Shown);

    // A program started and kept in front of the person, stood by until it
    // ends or is let go. Not started: the kernel's refusal, a -error_t.
    std::expected<Settled, int> InFront(Start What);

    // A program started to live behind. Not started: a -error_t.
    std::expected<void, int> Behind(Start What);

    // A program living behind brought forward, and stood by as one in front.
    // -ERR_PROCESS_NOT_FOUND when it lives behind no more.
    std::expected<Settled, int> Forward(std::uint32_t Pid, std::uint32_t Generation);

    // The programs living behind, the oldest first — not those that serve.
    std::vector<BehindNow> Living() const;

    // What the newest program that finished behind gave back to be kept as
    // `them`, taken; nothing when none gave anything since.
    std::optional<std::vector<file_id_t>> TakeGift();

    // How the person set the hub, for what the keeping strand says.
    void Show(const Look &Shown);

private:
    struct Job {
        Start         What;
        std::uint32_t Pid = 0;
        std::uint32_t Generation = 0;
        bool          Front = false;      // the prompt stands by for it
        bool          Wants = false;      // behind, it asked for the keyboard
        bool          Over = false;       // its end was heard
        bool          LetGo = false;      // in front, it was let go: the prompt is told
        Ending        End;
    };
    struct Asked {
        Start         What;
        bool          Front = false;
        bool          Done = false;
        int           Rc = 0;
        std::uint32_t Pid = 0;
        std::uint32_t Generation = 0;
    };

    void Keep();
    bool StartAsked();
    bool HearDeaths();
    bool HearDisplay();
    bool HearWake();
    void Died(const TouchProcessDied &Death);
    void Pressed(std::uint8_t What, std::uint32_t Pid, std::uint32_t Generation);
    std::expected<void, int> Ask(Asked &One);
    std::expected<Settled, int> StandBy(std::uint32_t Pid, std::uint32_t Generation);

    Look                                  Seen{};   // the keeping strand's own copy
    mutable std::mutex                    Lock;     // all of the below
    std::condition_variable               Settle;
    std::vector<Job>                      Running;
    std::deque<Asked *>                   Starts;
    std::optional<std::vector<file_id_t>> Gift;
    TouchTag                              Deaths = TOUCH_TAG_INVALID;
    TouchTag                              News = TOUCH_TAG_INVALID;
    TouchTag                              Woken = TOUCH_TAG_INVALID;
    TouchTagPair                          WakeTag{};
    std::uint32_t                         DisplayPid = 0;   // who says the display's news
};

}  // namespace hub

#endif
