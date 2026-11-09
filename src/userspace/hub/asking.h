/* asking.h — the questions the hub puts to the person: the ones a Form's
 * filler raises, and the hub's own.
 *
 * One look for all of them: what is asked, the choices numbered, a short
 * prompt. An empty answer, no one to answer, or the hub asked to finish ends
 * the question, and the caller does not go on. */
#ifndef HUB_ASKING_H
#define HUB_ASKING_H

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "box/types.h"

namespace hub::asking {

// Answered, answered with nothing, asked of no one (the console's ear is
// gone), or not answered because the hub was asked to finish (Ctrl+Alt+End
// over it): it reads no more, and ends.
enum class Answer { Given, Empty, NoOne, Ended };

// The prompt said and one line read, blanks trimmed off both ends.
Answer Line(std::string_view Prompt, std::string &Text);

// Heading, then each of Choices numbered, then "which one? (a number)" —
// asked again for a number not on the list. Chosen is the index of the one.
Answer Choose(std::string_view Heading, std::span<const std::string> Choices, std::size_t &Chosen);

// "<word> could be any of these:", each file numbered and wearing its tags.
Answer Which(std::string_view Word, std::span<const file_id_t> Ids, file_id_t &Chosen);

// "<question> (yes or no)" — asked again for anything else.
Answer YesNo(std::string_view Question, bool &Yes);

// "all is the whole volume here: N files. sure? (yes or no)"
Answer Sure(std::size_t Count, bool &Yes);

// Who was not started because the question ended as it did — nothing said
// when the hub is ending.
void SayCancelled(std::string_view Who, Answer Ended);

}  // namespace hub::asking

#endif
