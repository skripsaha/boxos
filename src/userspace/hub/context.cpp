/* context.cpp — the Use Context read whole, however long it has grown. */

#include "context.h"

#include "box/use.h"

namespace hub::context {

std::optional<std::string> Whole()
{
    // Asked for its size first, and read again if it grew in between.
    for (;;) {
        std::size_t Needed = 0;
        if (use_get(nullptr, 0, &Needed) < 0) return std::nullopt;
        if (Needed <= 1) return std::string();

        std::string Tags(Needed, '\0');
        std::size_t Again = 0;
        if (use_get(Tags.data(), Tags.size(), &Again) < 0) return std::nullopt;
        if (Again <= Needed) {
            Tags.resize(Again > 0 ? Again - 1 : 0);
            return Tags;
        }
    }
}

}  // namespace hub::context
