/* news.h — what the hub says of the programs it starts, each as the person
 * set it (look.h): how one ended when it did not end well, how long a command
 * took when it took long, one going behind, and one behind asking for the
 * keyboard. Of one behind that ends nothing is said: it lives on its own. */
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

// "edit lives on its own behind the hub: ctrl+alt+v brings it back" — let go
// by the person, the key next as they set it.
void LetGo(const Look &Shown, std::string_view Who);

// "edit asks for the keyboard: ctrl+alt+v brings it"
void Asks(const Look &Shown, std::string_view Who);

// "edit lives on behind this hub now: ctrl+alt+v brings it" — given by the
// display when the hub that let it go ended.
void Given(const Look &Shown, std::string_view Who);

// "took 12 s", "took 3 min 20 s", "took 1 h 2 min" — when it is as long as
// the person set, or longer.
void Took(const Look &Shown, std::chrono::steady_clock::duration Spent);

}  // namespace hub::news

#endif
