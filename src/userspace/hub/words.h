/* words.h — a typed line cut into words.
 *
 * The cut is the Luggage's (box/luggage.h, luggage_cut): blanks separate, a
 * word that begins with a double quote runs to the next one. One rule, so a
 * built-in sees the same words a program finds in its Luggage, and the rest
 * of a line handed to a program begins where its word begins. */
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
    Words(const Words &)            = delete;   // the words point into the cut it holds
    Words &operator=(const Words &) = delete;

    std::size_t      Count() const noexcept { return List.size(); }
    bool             Empty() const noexcept { return List.empty(); }
    // Word At, its quotes left off.
    std::string_view operator[](std::size_t At) const noexcept { return List[At]; }

    // The line from word At to its end, as typed — blanks and quotes kept;
    // "" past the last word.
    std::string_view Tail(std::size_t At) const noexcept;

private:
    std::string         Whole;     // the line as typed
    std::string         Cut;       // the same, cut in place
    std::vector<char *> List;
};

}  // namespace hub

#endif
