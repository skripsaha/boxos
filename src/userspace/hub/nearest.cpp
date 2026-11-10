/* nearest.cpp — edit distance, and the closest names of the volume's commands. */

#include "nearest.h"

#include <algorithm>
#include <vector>

#include "builtins.h"
#include "programs.h"
#include "words.h"

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

std::vector<std::string> Names(std::string_view Typed)
{
    if (Typed.empty()) return {};
    Closest Found;
    Found.Limit = Typed.size() <= Short ? 1 : 2;
    for (const Builtin &One : Builtins()) Found.Consider(Typed, One.Name);
    programs::EachCommand([&](file_id_t, std::string_view FileName) {
        Found.Consider(Typed, FileName.substr(0, programs::StemLength(FileName)));
    });
    return Found.Names;
}

std::string Said(const Guess &Guessed, bool Offered)
{
    const std::vector<std::string> &Names = Guessed.Names;
    const bool                      Several = Names.size() > 1;
    std::string                     Line  = "did you mean ";
    for (std::size_t I = 0; I < Names.size(); I++) {
        if (I > 0) Line += I + 1 == Names.size() ? " or " : ", ";
        if (Offered && Several) Line += std::to_string(I + 1) + " ";
        Line += Names[I];
    }
    Line += "?";
    if (Offered) Line += Several ? " yes and its number runs it" : " yes runs it";
    return Line;
}

Answer Answered(const Guess &Guessed, std::string_view Line)
{
    const Words Cut(Line);
    if (Cut.Empty() || Cut[0] != "yes" || Cut.Count() > 2 || Guessed.Names.empty()) return {};
    std::size_t Chosen = 1;
    if (Cut.Count() == 2) {
        const std::string_view Number = Cut[1];
        if (Number.size() != 1 || Number[0] < '1' || Number[0] > '0' + static_cast<int>(Guessed.Names.size()))
            return {};
        Chosen = static_cast<std::size_t>(Number[0] - '0');
    } else if (Guessed.Names.size() > 1) {
        return Answer{Answer::Kind::Which, {}};
    }
    std::string Meant = Guessed.Names[Chosen - 1];
    if (!Guessed.Rest.empty()) Meant += " " + Guessed.Rest;
    return Answer{Answer::Kind::Meant, std::move(Meant)};
}

}  // namespace hub::nearest
