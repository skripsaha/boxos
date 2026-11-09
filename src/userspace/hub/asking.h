/* asking.h — the questions the hub puts to the person while it fills a Form.
 *
 * One look for all of them: what is asked, the choices numbered, a short
 * prompt. An empty answer, or no one to answer, ends the question, and the
 * caller starts nothing. */
#ifndef HUB_ASKING_H
#define HUB_ASKING_H

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "box/types.h"

namespace hub::asking {

// Answered, answered with nothing, or asked of no one (the console's ear is gone).
enum class Answer { Given, Empty, NoOne };

// The prompt said and one line read, blanks trimmed off both ends.
Answer Line(std::string_view Prompt, std::string &Text);

// "<word> could be any of these:", each file numbered and wearing its tags,
// then "which one? (a number)" — asked again for a number not on the list.
Answer Which(std::string_view Word, std::span<const file_id_t> Ids, file_id_t &Chosen);

// "all is the whole volume here: N files. sure? (yes or no)"
Answer Sure(std::size_t Count, bool &Yes);

// Who was not started because the question ended as it did.
void SayCancelled(std::string_view Who, Answer Ended);

}  // namespace hub::asking

#endif
