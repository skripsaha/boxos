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
        int         Rc = use_get(Tags.data(), Tags.size(), &Again);
        if (Rc < 0) return std::nullopt;
        if (Again <= Needed) {
            Tags.resize(Again > 0 ? Again - 1 : 0);
            return Tags;
        }
    }
}

std::size_t Count(std::string_view Tags)
{
    if (Tags.empty()) return 0;
    std::size_t Commas = 0;
    for (char C : Tags) Commas += C == ',';
    return Commas + 1;
}

bool Holds(std::string_view Tags, std::string_view Tag)
{
    while (!Tags.empty()) {
        const std::size_t Comma = Tags.find(',');
        if (Tags.substr(0, Comma) == Tag) return true;
        if (Comma == std::string_view::npos) break;
        Tags.remove_prefix(Comma + 1);
    }
    return false;
}

bool Same(std::string_view Left, std::string_view Right)
{
    if (Count(Left) != Count(Right)) return false;
    while (!Left.empty()) {
        const std::size_t Comma = Left.find(',');
        if (!Holds(Right, Left.substr(0, Comma))) return false;
        if (Comma == std::string_view::npos) break;
        Left.remove_prefix(Comma + 1);
    }
    return true;
}

}  // namespace hub::context
