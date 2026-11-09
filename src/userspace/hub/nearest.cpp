/* nearest.cpp — edit distance, and the closest names of the volume's commands. */

#include "nearest.h"

#include <algorithm>
#include <vector>

#include "builtins.h"
#include "programs.h"

namespace hub::nearest {

namespace {

constexpr std::size_t Shown = 3;
constexpr std::size_t Short = 3;

// Edit distance of two names with a swap of neighbours as one step.
std::size_t Distance(std::string_view A, std::string_view B)
{
    std::vector<std::size_t> Before(B.size() + 1), Last(B.size() + 1), Row(B.size() + 1);
    for (std::size_t J = 0; J <= B.size(); J++) Last[J] = J;
    for (std::size_t I = 1; I <= A.size(); I++) {
        Row[0] = I;
        for (std::size_t J = 1; J <= B.size(); J++) {
            const std::size_t Same = A[I - 1] == B[J - 1] ? 0 : 1;
            Row[J] = std::min({Last[J] + 1, Row[J - 1] + 1, Last[J - 1] + Same});
            if (I > 1 && J > 1 && A[I - 1] == B[J - 2] && A[I - 2] == B[J - 1])
                Row[J] = std::min(Row[J], Before[J - 2] + 1);
        }
        Before.swap(Last);
        Last.swap(Row);
    }
    return Last[B.size()];
}

struct Closest {
    std::vector<std::string> Names;
    std::size_t              Best = static_cast<std::size_t>(-1);
    std::size_t              Limit;

    void Consider(std::string_view Typed, std::string_view Name)
    {
        if (Name.empty()) return;
        const std::size_t Far = Distance(Typed, Name);
        if (Far == 0 || Far > Limit || Far > Best) return;
        if (Far < Best) {
            Best = Far;
            Names.clear();
        }
        if (std::find(Names.begin(), Names.end(), Name) != Names.end()) return;
        if (Names.size() < Shown) Names.emplace_back(Name);
    }
};

}  // namespace

std::optional<std::string> For(std::string_view Typed)
{
    if (Typed.empty()) return std::nullopt;
    Closest Found;
    Found.Limit = Typed.size() <= Short ? 1 : 2;
    for (const Builtin &One : Builtins()) Found.Consider(Typed, One.Name);
    programs::EachCommand([&](file_id_t, std::string_view FileName) {
        Found.Consider(Typed, FileName.substr(0, programs::StemLength(FileName)));
    });
    if (Found.Names.empty()) return std::nullopt;

    std::string Said = "did you mean ";
    for (std::size_t I = 0; I < Found.Names.size(); I++) {
        if (I > 0) Said += I + 1 == Found.Names.size() ? " or " : ", ";
        Said += Found.Names[I];
    }
    return Said + "?";
}

}  // namespace hub::nearest
