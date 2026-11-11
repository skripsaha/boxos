/* lore.h — what the wizard knows: the charms he casts, the things he puts in
 * the cauldron, the potions they make together, the words he says. No screen
 * in it, and no display: a Casting is what is to be laid over the glass, and
 * the tower lays it. */
#ifndef WIZARD_LORE_H
#define WIZARD_LORE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "look.h"

namespace wizard {

// The wizard's luck: the same on no two days.
class Dice {
public:
    explicit Dice(std::uint32_t Seed) noexcept : State(Seed ? Seed : 0x9E3779B9u) {}
    std::uint32_t Next() noexcept;
    // 0 up to Count, not including it.
    std::uint32_t Below(std::uint32_t Count) noexcept { return Count ? Next() % Count : 0; }

private:
    std::uint32_t State;
};

// How his face takes what he has done.
enum class Mood : std::uint8_t { Pleased, Surprised, Dizzy, Spooked, Squinting };

// A charm: one way to change the glass, with the words that go with it. Word
// is how it is named on the command line.
struct Charm {
    std::string_view          Word;
    std::string_view          Name;
    std::uint32_t             Kinds;     // DISPLAY_EFFECT_*
    std::uint8_t              Axes;      // DISPLAY_EFFECT_AXIS_*
    std::uint32_t             Tint;
    Mood                      Face;
    std::array<std::string_view, 2> Closings;
};

// A thing for the cauldron.
struct Ingredient {
    std::string_view Word;
    std::string_view Name;
    std::string_view Spirit;             // what it adds, said in a potion's name
    std::string_view Looks;              // what it is, in a few words
    std::uint32_t    Kinds;
    std::uint32_t    Color;              // what it makes of the liquid
};

// A potion that is more than its parts: the things that make it, whichever
// order they went in, and what it adds. A person learns it by brewing it.
struct Recipe {
    std::string_view          Word;
    std::string_view          Name;
    std::array<std::uint8_t, 3> Of;       // places in Ingredients(); NoIngredient where there are fewer
    std::uint32_t             Kinds;      // added to the parts'
    std::uint32_t             Color;      // the liquid, in place of the mean of the parts'
    std::string_view          Clue;       // what is said of it before it is found
    std::string_view          Verse;
};

inline constexpr std::size_t CharmCount = 24;
inline constexpr std::size_t IngredientCount = 16;
inline constexpr std::size_t RecipeCount = 10;
// The most the cauldron holds.
inline constexpr std::size_t CauldronMost = 4;

const std::array<Charm, CharmCount> &Charms() noexcept;
const std::array<Ingredient, IngredientCount> &Ingredients() noexcept;
const std::array<Recipe, RecipeCount> &Recipes() noexcept;

const Charm *CharmNamed(std::string_view Word) noexcept;
// The place in Ingredients() of the one named, or none.
std::size_t IngredientNamed(std::string_view Word) noexcept;
inline constexpr std::size_t NoIngredient = static_cast<std::size_t>(-1);
inline constexpr std::uint8_t NoThing = 0xFF;

// What is to be laid over the glass, and what the wizard says of it. A Casting
// with no Kinds is one the person's settings leave nothing of.
struct Casting {
    const Charm  *Of = nullptr;   // the charm, when it was one
    std::size_t   Found = NoIngredient;   // the recipe, when the potion was one (a place in Recipes())
    std::string   Name;
    std::string   Verse;
    std::uint32_t Kinds = 0;
    std::uint8_t  Strength = 0;
    std::uint8_t  Axes = 0;
    std::uint32_t Color = 0;
    std::uint32_t DurationMs = 0;
    Mood          Face = Mood::Pleased;
};

// A charm of the allowed ones, chosen by chance — not the one Last, when there
// is another; with none allowed, any.
const Charm &ChooseCharm(Dice &Luck, const Look &Shown, const Charm *Last);
// The named charm, said over.
Casting Spell(const Charm &Which, Dice &Luck, const Look &Shown);

// What the things in the cauldron make, by their places in Ingredients(), and
// what the wizard says as it goes down.
Casting Potion(std::span<const std::uint8_t> Mixed, Dice &Luck, const Look &Shown);

// The recipe the things make, as a place in Recipes(), or none.
std::size_t RecipeOf(std::span<const std::uint8_t> Mixed) noexcept;

// What the liquid looks like: the mean of the colors of what is in it.
std::uint32_t Liquid(std::span<const std::uint8_t> Mixed) noexcept;

// The kinds of effect as words, "sway, tint and snow".
std::string EffectNames(std::uint32_t Kinds);

// What the wizard says as a thing goes in, and of the brew so far.
std::string Remark(std::size_t Which, std::span<const std::uint8_t> Mixed, Dice &Luck);

}  // namespace wizard

#endif
