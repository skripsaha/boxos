/* paint_words.h — what the board's pictures say, in words and colours: the
 * one wording of a state, a size, a time, a share, an order, and the colour of
 * where a program lives and of what happened — the cells' picture's
 * (paint.cpp) and the items' (picture.cpp) alike. */

#ifndef TASKBOARD_PAINT_WORDS_H
#define TASKBOARD_PAINT_WORDS_H

#include <cstdint>
#include <string>
#include <string_view>

#include "journal.h"
#include "look.h"
#include "programs.h"

namespace taskboard {

// Between the words of a line: a middle dot, as text.
inline constexpr std::string_view Between = "\xC2\xB7";

Color       LivingInk(const Look &Shown, Living Where);
Color       HappenedInk(const Look &Shown, Happened What);
const char *StateName(std::uint32_t State);
std::string Bytes(std::uint64_t Count);
std::string Lasted(std::uint64_t Us);
std::string Percent(double Share);
const char *SortName(SortBy Sort);

}  // namespace taskboard

#endif
