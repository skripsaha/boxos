/* news.h — what the hub says of the programs it starts, each as the person
 * set it (look.h): how one ended when it did not end well, how long a command
 * took when it took long, and what becomes of the programs living behind. */
#ifndef HUB_NEWS_H
#define HUB_NEWS_H

#include <chrono>
#include <string_view>

#include "jobs.h"
#include "look.h"

namespace hub::news {

// "edit ended: 3", "checksum stopped: page fault in Hash". A program that
// ended well is not spoken of, nor one the person asked to finish and that
// was ended for it.
void Ended(const Look &Shown, std::string_view Who, const Ending &End);

// "edit lives on behind: front edit brings it back" — let go by the person.
void LetGo(const Look &Shown, std::string_view Who);

// A program that lived behind ended: "radio finished", "radio finished; gave
// 2 files — them", or how it ended when it did not end well. One that serves
// is spoken of only then.
void Finished(const Look &Shown, std::string_view Who, bool Serves, const Ending &End);

// "edit wants the keyboard: front edit brings it forward"
void Wants(const Look &Shown, std::string_view Who);

// "took 12 s", "took 3 min 20 s", "took 1 h 2 min" — when it is as long as
// the person set, or longer.
void Took(const Look &Shown, std::chrono::steady_clock::duration Spent);

}  // namespace hub::news

#endif
