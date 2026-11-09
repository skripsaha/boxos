/* words.h — a typed line cut into words.
 *
 * The cut is the Luggage's (box/luggage.h): blanks separate, "double quotes"
 * group. One rule, so a built-in sees the same words a program finds in its
 * Luggage. */
#ifndef HUB_WORDS_H
#define HUB_WORDS_H

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace hub {

class Words {
public:
    explicit Words(std::string_view Line);

    std::size_t      Count() const noexcept { return List.size(); }
    bool             Empty() const noexcept { return List.empty(); }
    std::string_view operator[](std::size_t At) const noexcept { return List[At]; }

private:
    std::string        Storage;   // the line, cut in place
    std::vector<char *> List;
};

// What is left of a line after its first Skip words, cut as Words cuts them,
// the blanks before the next word dropped.
std::string_view Rest(std::string_view Line, std::size_t Skip);

}  // namespace hub

#endif
