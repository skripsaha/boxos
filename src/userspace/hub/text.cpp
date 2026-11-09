/* text.cpp — trimming and comparing typed words. */

#include "text.h"

namespace hub::text {

std::string_view Trimmed(std::string_view Text) noexcept
{
    while (!Text.empty() && Blank(Text.front())) Text.remove_prefix(1);
    while (!Text.empty() && Blank(Text.back())) Text.remove_suffix(1);
    return Text;
}

bool Says(std::string_view Typed, std::string_view Word) noexcept
{
    if (Typed.size() != Word.size()) return false;
    for (std::size_t I = 0; I < Typed.size(); I++) {
        char C = Typed[I];
        if (C >= 'A' && C <= 'Z') C = static_cast<char>(C - 'A' + 'a');
        if (C != Word[I]) return false;
    }
    return true;
}

}  // namespace hub::text
