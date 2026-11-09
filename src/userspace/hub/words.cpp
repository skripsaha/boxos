/* words.cpp — the Luggage's cut, counted first and then laid into a list of
 * exactly that many words. */

#include "words.h"

#include "box/luggage.h"

namespace hub {

Words::Words(std::string_view Line) : Whole(Line), Cut(Line)
{
    // Counted on the copy, which the count cuts; then the copy is laid down
    // again and cut into a list of exactly that many words.
    const std::uint32_t Total = luggage_cut(Cut.data(), nullptr, 0);
    Cut.assign(Line);
    List.resize(Total);
    if (Total > 0) luggage_cut(Cut.data(), List.data(), Total);
}

std::string_view Words::Tail(std::size_t At) const noexcept
{
    if (At >= List.size()) return {};
    // The cut writes a NUL where a word ends and nothing before one, so a
    // word stands where it stood in the line, and one opened by a quote has
    // that quote still before it.
    std::size_t Start = static_cast<std::size_t>(List[At] - Cut.data());
    if (Start > 0 && Cut[Start - 1] == '"') Start--;
    return std::string_view(Whole).substr(Start);
}

}  // namespace hub
