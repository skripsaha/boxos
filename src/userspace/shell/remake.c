/*
 * remake.c — a program made here starts as what its files make now.
 *
 * The shell reads the program's Recipe (box/recipe.h) and every file in it
 * again. A file that changed since the program was made: the tool the Recipe
 * names makes it again — the program handed to it as `it` — and the program
 * made is the one that starts; when it cannot be made, nothing starts, for the
 * person asked for the program their files make, not an older one. A file
 * that is gone makes nothing: the program starts as it was made, and the
 * person is told. No Recipe — the system's own programs, built on the host —
 * starts as it is. A program made again takes its own place, in `them` too.
 */

#include "remake.h"
#include "fill.h"
#include "program.h"
#include "them.h"
#include "box/recipe.h"
#include "box/print.h"
#include "box/memory.h"
#include "box/error.h"

static size_t CountIn(const BoxRecipe *recipe, const uint8_t *states, uint8_t state, size_t first)
{
    size_t count = 0;
    for (size_t i = first; i < recipe->FileCount; i++) count += states[i] == state;
    return count;
}

/* The names of the files in State, from First on, Count of them: "a",
 * "a and b", "a, b and c". */
static void SayNames(const BoxRecipe *recipe, const uint8_t *states, uint8_t state, size_t first,
                     size_t count)
{
    size_t said = 0;
    for (size_t i = first; i < recipe->FileCount; i++) {
        if (states[i] != state) continue;
        if (said > 0) print(said + 1 == count ? " and " : ", ");
        print(recipe->Files[i].Name);
        said++;
    }
}

/* The tool the Recipe names, started on the program; *program the one it
 * gave back. */
static bool MakeAgain(const BoxRecipe *recipe, file_id_t *program, const char *who)
{
    file_id_t tool;
    int picked = ProgramPick(recipe->By, &tool);
    if (picked == PROGRAM_SAID) return false;
    if (picked != PROGRAM_PICKED) {
        printf("%s: not started: %s, which made it, is not here to make it again\n", who, recipe->By);
        return false;
    }

    file_id_t made;
    if (!FillAndRunOn(tool, recipe->By, *program, &made)) {
        printf("%s: not started: it could not be made again\n", who);
        return false;
    }
    if (made == 0) {
        printf("%s: not started: %s gave back no program\n", who, recipe->By);
        return false;
    }
    ThemReplace(*program, made);
    *program = made;
    return true;
}

static bool FollowStates(const BoxRecipe *recipe, const uint8_t *states, file_id_t *program,
                         const char *who)
{
    if (states[0] == RECIPE_GONE) {
        printf("%s: %s, which it was made from, is gone; started as it was made\n", who,
               recipe->Files[0].Name);
        return true;
    }

    size_t changed = CountIn(recipe, states, RECIPE_CHANGED, 0);
    size_t gone    = CountIn(recipe, states, RECIPE_GONE, 1);
    if (changed > 0) {
        printf("%s: ", who);
        SayNames(recipe, states, RECIPE_CHANGED, 0, changed);
        println(" changed since it was made; making it again");
        return MakeAgain(recipe, program, who);
    }
    if (gone > 0) {
        printf("%s: ", who);
        SayNames(recipe, states, RECIPE_GONE, 1, gone);
        printf(", which it was made from, %s gone; started as it was made\n", gone == 1 ? "is" : "are");
    }
    return true;
}

bool RemakeWhenChanged(file_id_t *program, const char *who)
{
    BoxRecipe recipe;
    int rc = recipe_of_file(*program, &recipe);
    if (rc != 0) {
        printf("%s: its Recipe cannot be read (error %d); started as it is\n", who,
               (int)box_errno_of(rc));
        return true;
    }
    if (!recipe.Declared) return true;

    uint8_t *states = NULL;
    rc = recipe_check(&recipe, &states);
    bool start = true;
    if (rc != 0) {
        printf("%s: the files it was made from cannot be read (error %d); started as it was made\n",
               who, (int)box_errno_of(rc));
    } else {
        start = FollowStates(&recipe, states, program, who);
    }
    free(states);
    recipe_release(&recipe);
    return start;
}
