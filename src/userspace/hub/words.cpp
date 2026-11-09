/* words.cpp — the Luggage's cut, counted first and then laid into a list of
 * exactly that many words. */

#include "words.h"

#include "box/luggage.h"

namespace hub {

namespace {

bool Blank(char C)
{
    return C == ' ' || C == '\t';
}

}  // namespace

Words::Words(std::string_view Line) : Storage(Line)
{
    // Counted on the copy, which the count cuts; then the copy is laid down
    // again and cut into a list of exactly that many words.
    const std::uint32_t Count = luggage_cut(Storage.data(), nullptr, 0);
    Storage.assign(Line);
    List.resize(Count);
    if (Count > 0) luggage_cut(Storage.data(), List.data(), Count);
}

std::string_view Rest(std::string_view Line, std::size_t Skip)
{
    std::size_t At = 0;
    for (std::size_t Skipped = 0; Skipped < Skip; Skipped++) {
        while (At < Line.size() && Blank(Line[At])) At++;
        bool Quoted = false;
        while (At < Line.size() && (Quoted || !Blank(Line[At]))) {
            if (Line[At] == '"') Quoted = !Quoted;
            At++;
        }
    }
    while (At < Line.size() && Blank(Line[At])) At++;
    return Line.substr(At);
}

}  // namespace hub
