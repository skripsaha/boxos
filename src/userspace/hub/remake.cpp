/* remake.cpp — the Recipe read, its files checked, the program made again. */

#include "remake.h"

#include <cstdint>

#include "box/recipe.h"

#include "held.h"
#include "hub.h"
#include "launch.h"
#include "programs.h"
#include "say.h"

namespace hub::remake {

namespace {

std::size_t CountIn(const BoxRecipe &Recipe, const std::uint8_t *States, std::uint8_t State, std::size_t First)
{
    std::size_t Count = 0;
    for (std::size_t I = First; I < Recipe.FileCount; I++) Count += States[I] == State;
    return Count;
}

// The names of the files in State, from First on, Count of them: "a",
// "a and b", "a, b and c".
void SayNames(const BoxRecipe &Recipe, const std::uint8_t *States, std::uint8_t State, std::size_t First,
              std::size_t Count)
{
    std::size_t Said = 0;
    for (std::size_t I = First; I < Recipe.FileCount; I++) {
        if (States[I] != State) continue;
        if (Said > 0) Say(Said + 1 == Count ? " and " : ", ");
        Say(Recipe.Files[I].Name);
        Said++;
    }
}

// The tool the Recipe names, started on the program; Program the one it gave back.
bool MakeAgain(Hub &Self, const BoxRecipe &Recipe, file_id_t &Program, std::string_view Who)
{
    programs::Picked Tool = programs::Pick(Recipe.By);
    if (!Tool && Tool.error() == programs::Said) return false;
    if (!Tool) {
        Say("{}: not started: {}, which made it, is not here to make it again\n", Who, Recipe.By);
        return false;
    }
    file_id_t Made = 0;
    if (!launch::OnThing(Self, *Tool, Recipe.By, Program, Made)) {
        Say("{}: not started: it could not be made again\n", Who);
        return false;
    }
    if (Made == 0) {
        Say("{}: not started: {} gave back no program\n", Who, Recipe.By);
        return false;
    }
    Self.Given.Replace(Program, Made);
    Program = Made;
    return true;
}

bool FollowStates(Hub &Self, const BoxRecipe &Recipe, const std::uint8_t *States, file_id_t &Program,
                  std::string_view Who)
{
    if (States[0] == RECIPE_GONE) {
        Say("{}: {}, which it was made from, is gone; started as it was made\n", Who, Recipe.Files[0].Name);
        return true;
    }
    const std::size_t Changed = CountIn(Recipe, States, RECIPE_CHANGED, 0);
    const std::size_t Gone    = CountIn(Recipe, States, RECIPE_GONE, 1);
    if (Changed > 0) {
        Say("{}: ", Who);
        SayNames(Recipe, States, RECIPE_CHANGED, 0, Changed);
        Say(" changed since it was made; making it again\n");
        return MakeAgain(Self, Recipe, Program, Who);
    }
    if (Gone > 0) {
        Say("{}: ", Who);
        SayNames(Recipe, States, RECIPE_GONE, 1, Gone);
        Say(", which it was made from, {} gone; started as it was made\n", Gone == 1 ? "is" : "are");
    }
    return true;
}

}  // namespace

bool WhenChanged(Hub &Self, file_id_t &Program, std::string_view Who)
{
    HeldRecipe Recipe;
    int        Rc = Recipe.Read(Program);
    if (Rc != 0) {
        Say("{}: its Recipe cannot be read ({}); started as it is\n", Who, Reason(Rc));
        return true;
    }
    if (!Recipe->Declared) return true;

    std::uint8_t *States = nullptr;
    Rc = recipe_check(&*Recipe, &States);
    const HeldHeap<std::uint8_t> Checked(States);
    if (Rc != 0) {
        Say("{}: the files it was made from cannot be read ({}); started as it was made\n", Who, Reason(Rc));
        return true;
    }
    return FollowStates(Self, *Recipe, States, Program, Who);
}

}  // namespace hub::remake
