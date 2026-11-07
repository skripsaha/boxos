#ifndef BOXOS_DISPLAY_RING_H
#define BOXOS_DISPLAY_RING_H

#include <cstdint>

#include "console_model.h"

// The log ring, followed (box/screen.h). The ring is the machine's one account
// of what it said: the kernel's lines, and — because the display says every
// run it draws into it — the programs' text too, in the order the screen shows
// it. The screen IS the ring's order. A program's text is said first and drawn
// second: the run gets its place in the ring, the kernel's lines before that
// place are drawn, then the run, so the screen and the serial line and
// `logsave` never disagree about what came before what.
class RingFollower {
public:
    // The kernel's lines up to `End` (or to what the ring holds, if less) into
    // the model. True when anything was drawn. A ring the kernel will not let
    // this program read is said so once and never read again.
    bool CatchUp(ConsoleModel &Model, std::uint64_t End) noexcept;

    // The same up to the ring's live end — read only when there is cause to
    // look: at the start, and once the bell has answered that something was
    // said past what was seen. Every read is a call into the kernel, and its
    // answer is an arrival that would refuse the display's sleep, so the ring
    // is never read on the chance.
    bool CatchUp(ConsoleModel &Model) noexcept;

    // A run of the display's own text into the ring. [Start, End) is where it
    // went; false when the kernel would not take it, which is said once.
    bool Say(const char *Bytes, std::uint32_t Count, std::uint64_t &Start,
             std::uint64_t &End) noexcept;

    // The display's own run is drawn: the ring is followed past it.
    void Passed(std::uint64_t End) noexcept;

    // Before sleeping, and before the mark the sleep is taken against: the
    // bell hung at what has been seen. True when an utterance past it is
    // there already — no bell was hung, the ring is due to be read, do not
    // sleep. An unreadable ring hangs no bell: news of it could not be read.
    bool HangBell() noexcept;

private:
    std::uint64_t Position = 0;   // drawn up to here
    std::uint64_t Seen = 0;       // looked at up to here: past Position only by
                                  // a character still waiting for its last bytes
    bool Due = true;
    bool Unreadable = false;
};

#endif
