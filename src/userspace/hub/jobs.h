/* jobs.h — the programs this hub starts, and how each ended.
 *
 * A program's end is learnt from the kernel's word that it died
 * (process:died), claimed before the first program starts, so no end can be
 * missed: a program that is over before the hub asks has said so already. */
#ifndef HUB_JOBS_H
#define HUB_JOBS_H

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "box/touch.h"
#include "box/types.h"

namespace hub {

struct Hub;

// How a started program ended, and what it gave back.
struct Ending {
    bool                   Seen = false;        // its end was seen: Code is its own
    std::int32_t           Code = 0;            // proc_exit.h: its exit, or why it was ended
    bool                   Fell = false;        // the kernel said how it fell: Vector, In
    std::uint8_t           Vector = 0;          // the exception it fell on
    std::string            In;                  // the function it fell in; "" unnamed
    bool                   Collected = false;   // what it queued was read off
    std::vector<file_id_t> Given;               // its Souvenir
};

class Jobs {
public:
    // Ears for every death, before anything is started.
    void Begin();

    // The program started with its Luggage, stood by until it is gone. Not
    // started: the kernel's refusal, a negative -error_t.
    std::expected<Ending, int> Run(file_id_t Program, std::string_view Luggage);

private:
    void Forget();
    Ending WaitGone(std::uint32_t Pid, std::uint32_t Generation);

    TouchTag Died = TOUCH_TAG_INVALID;
};

// How a program ended, said in the hub's words when it did not end well:
// "edit ended: 3", "checksum stopped: page fault in Hash". A program that
// ended well is not spoken of.
void SayEnding(const Hub &Self, std::string_view Who, const Ending &End);

}  // namespace hub

#endif
