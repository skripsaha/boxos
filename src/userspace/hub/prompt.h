/* prompt.h — what stands before the line the person types.
 *
 *   [code,cpp] 3 ~
 *
 * The Use Context in brackets, as the kernel holds it now; how many files
 * `them` holds; the sign. Each as the person set it (look.h): shown or not,
 * and its colour. As many whole tags as there is room for are shown, never
 * one torn mid-tag, and the rest are counted: "[code,cpp +3]". */
#ifndef HUB_PROMPT_H
#define HUB_PROMPT_H

#include <string_view>

namespace hub {

struct Hub;

// The prompt said for the Use Context Context (comma-joined, "" for none).
void ShowPrompt(const Hub &Self, std::string_view Context);

}  // namespace hub

#endif
