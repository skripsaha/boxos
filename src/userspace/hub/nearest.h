/* nearest.h — the names close to a command that was not found, and the one
 * line after it that may take one: `yes`.
 *
 * Closeness is the edit distance with a swap of two neighbours counted as one
 * step: at most 2, and at most 1 for a word of three letters or fewer, which
 * is near to everything otherwise. Only the closest names are kept, three at
 * most. A program's name is its file's name up to the last dot, as the volume
 * stamps it.
 *
 * What is guessed for a line typed at the prompt is offered for the next one
 * alone: `yes` — `yes 2` of several — runs the name meant with the rest of
 * that line, and any other line lets the guess go. `yes` is no command: it
 * means this only right after a guess. */
#ifndef HUB_NEAREST_H
#define HUB_NEAREST_H

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hub::nearest {

// The built-ins and the programs on the volume closest to Typed; none when
// none is close.
std::vector<std::string> Names(std::string_view Typed);

// The names guessed for a command not found, and the rest of the line it was
// typed with.
struct Guess {
    std::vector<std::string> Names;
    std::string              Rest;
};

// "did you mean files?"; offered for the next line, "did you mean files? yes
// runs it", "did you mean 1 clock, 2 lock or 3 look? yes and its number runs
// it".
std::string Said(const Guess &Guessed, bool Offered);

// What the next line makes of a guess offered: the line it stands for — the
// name meant and the rest — a question which one when `yes` alone answers
// several, or nothing when the line is no answer.
struct Answer {
    enum class Kind { None, Meant, Which } Is = Kind::None;
    std::string Line;
};
Answer Answered(const Guess &Guessed, std::string_view Line);

}  // namespace hub::nearest

#endif
