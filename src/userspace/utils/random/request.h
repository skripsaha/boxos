/* request.h — what the person asked for, read from the words and files the
 * Form filled:
 *
 *   [shuffle | N of | N different of]  [ A B | N | word word ... ]
 *
 * The first part says how many: `shuffle` all of what there is, in a new
 * order; `N of` N draws, each as likely as the others whatever came before;
 * `N different of` N draws no one of which comes twice; none, one draw. The
 * rest says what from: two whole numbers, a range; one, a range from 1 to it;
 * none, the range set in the settings; anything else — three numbers too —
 * words to choose among. Files, where files were given, are what is chosen
 * among, and nothing else is said then. */
#ifndef RANDOM_REQUEST_H
#define RANDOM_REQUEST_H

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "box/types.h"

namespace random_pick {

enum class Kind { Range, Words, Files };

struct Request {
    Kind                     From = Kind::Range;
    std::int64_t             Low  = 1;
    std::int64_t             High = 100;
    std::vector<std::string> Names;     // the words, or the names of the files
    std::vector<file_id_t>   Files;     // beside Names, for Files
    std::uint64_t            Count     = 1;
    bool                     Different = false;
    bool                     All       = false;   // as many as there are

    // How many there are to choose among.
    std::uint64_t Pool() const noexcept;
};

// The most that is drawn at once.
inline constexpr std::uint64_t MostDrawn = 100000;

struct Bounds {
    std::int64_t Low;
    std::int64_t High;
};

// The request, or a sentence that says what is wrong with it. Bounds are the
// range for when none is said.
std::expected<Request, std::string> Read(const std::vector<std::string> &Words, const std::vector<file_id_t> &Files,
                                         Bounds Fallback);

}  // namespace random_pick

#endif
