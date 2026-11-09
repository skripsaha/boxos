/* launch.h — a program run from its file, as far as it goes: made again when
 * what it was made from changed (remake.h), its Form filled (fill.h),
 * started and stood by until it is gone (jobs.h), how it ended said
 * (news.h), and what it gave back kept as `them` — the file its screen went
 * into, or the Souvenir the hub asked for. */
#ifndef HUB_LAUNCH_H
#define HUB_LAUNCH_H

#include <string_view>

#include "box/types.h"

namespace hub {

struct Hub;

namespace launch {

// Program run on Line, the words after its name as typed; Who the name the
// person used. True when it started and ended with 0.
bool Program(Hub &Self, file_id_t Program, std::string_view Who, std::string_view Line);

// Program run on the one file Thing, asking nobody, the person's `them`
// neither read nor changed: true when it ended with 0, Given then the first
// file it gave back, 0 for none.
bool OnThing(Hub &Self, file_id_t Program, std::string_view Who, file_id_t Thing, file_id_t &Given);

}  // namespace launch
}  // namespace hub

#endif
