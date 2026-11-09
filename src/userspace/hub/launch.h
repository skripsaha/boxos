/* launch.h — a program run from its file, as far as it goes: made again when
 * what it was made from changed (remake.h), its Form filled (fill.h), started
 * as it lives (box/life.h, jobs.h) — a task in front, stood by until it ends
 * or is let go; one that stays or serves behind, at once — how it ended said
 * (news.h), and what it gave back kept as `them`: the file its screen went
 * into, or the Souvenir the hub asked for. */
#ifndef HUB_LAUNCH_H
#define HUB_LAUNCH_H

#include <string_view>

#include "box/types.h"

namespace hub {

struct Hub;
struct BehindNow;

namespace launch {

// Program run on Line, the words after its name as typed; Who the name the
// person used. True when it started and lives on, or ended with 0.
bool Program(Hub &Self, file_id_t Program, std::string_view Who, std::string_view Line);

// Program run on the one file Thing, asking nobody, the person's `them`
// neither read nor changed, and stood by for however it lives: true when it
// ended with 0, Given then the first file it gave back, 0 for none.
bool OnThing(Hub &Self, file_id_t Program, std::string_view Who, file_id_t Thing, file_id_t &Given);

// A program living behind brought forward and stood by as one in front.
// True when it lives on, or ended with 0.
bool Forward(Hub &Self, const BehindNow &Which);

}  // namespace launch
}  // namespace hub

#endif
