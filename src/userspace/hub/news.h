/* news.h — what the hub says of the programs it starts, each as the person
 * set it (look.h): how one ended when it did not end well, and how long a
 * command took when it took long. */
#ifndef HUB_NEWS_H
#define HUB_NEWS_H

#include <chrono>
#include <string_view>

#include "jobs.h"
#include "look.h"

namespace hub::news {

// "edit ended: 3", "checksum stopped: page fault in Hash". A program that
// ended well is not spoken of.
void Ended(const Look &Shown, std::string_view Who, const Ending &End);

// "took 12 s", "took 3 min 20 s", "took 1 h 2 min" — when it is as long as
// the person set, or longer.
void Took(const Look &Shown, std::chrono::steady_clock::duration Spent);

}  // namespace hub::news

#endif
