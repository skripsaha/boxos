/* lore.cpp — the charms, the things in the cauldron, and the words. */

#include "lore.h"

#include <algorithm>

#include "box/display.h"

namespace wizard {

namespace {

constexpr std::uint32_t Sway = DISPLAY_EFFECT_SWAY;
constexpr std::uint32_t Mirror = DISPLAY_EFFECT_MIRROR;
constexpr std::uint32_t Fade = DISPLAY_EFFECT_FADE;
constexpr std::uint32_t Invert = DISPLAY_EFFECT_INVERT;
constexpr std::uint32_t Tint = DISPLAY_EFFECT_TINT;
constexpr std::uint32_t Shimmer = DISPLAY_EFFECT_SHIMMER;
constexpr std::uint8_t  AxisX = DISPLAY_EFFECT_AXIS_X;
constexpr std::uint8_t  AxisY = DISPLAY_EFFECT_AXIS_Y;

const std::array<Charm, CharmCount> g_charms = {{
    {"giddy", "Giddy Sway", Sway | Tint, AxisX | AxisY, 0xB8E070,
     {"let the floor forget which way is down!", "let every line be a little tipsy!"}},
    {"mirror", "Looking-Glass", Mirror, AxisX, 0xFFFFFF,
     {"show the world its other face!", "let right be left for a while!"}},
    {"upside", "Upside-Down Tuesday", Mirror, AxisY, 0xFFFFFF,
     {"turn the ceiling into a floor!", "let the sky stand on its head!"}},
    {"ghost", "Ghost Colors", Invert, 0, 0xFFFFFF,
     {"let the dark be light and the light be dark!", "paint the night in the colors of day!"}},
    {"grey", "Grey Monday", Fade, 0, 0xFFFFFF,
     {"drain the color from the day!", "let every hue go quiet!"}},
    {"ember", "Ember Glass", Tint | Shimmer, 0, 0xFF7030,
     {"set the air alight without a flame!", "warm the glass with a dragon's breath!"}},
    {"frost", "Frostbite", Tint, 0, 0x60D8F0,
     {"breathe winter onto the glass!", "let the cold paint every word!"}},
    {"starfall", "Starfall", Shimmer, 0, 0xFFFFFF,
     {"rain the stars down on the room!", "let the night spill its sparkles!"}},
    {"moonlit", "Moonlit", Tint | Fade, 0, 0xB8C4FF,
     {"let the moon look in at the window!", "wash it all in silver!"}},
    {"fever", "Fever Dream", Sway | Tint | Shimmer, AxisX, 0xC070FF,
     {"let the room dream with its eyes open!", "stir the air like a spoon in a dream!"}},
}};

const std::array<Ingredient, IngredientCount> g_ingredients = {{
    {"toadstool", "Toadstool", "Giddiness", Sway, 0xB04030},
    {"moonwater", "Moonwater", "Pallor", Fade, 0xC8D0E0},
    {"pepper", "Dragon Pepper", "Fire", Tint, 0xE04020},
    {"berry", "Frost Berry", "Frost", Tint, 0x40C0D0},
    {"moss", "Glow Moss", "Glow", Tint, 0x50D050},
    {"stardust", "Star Dust", "Stars", Shimmer, 0xFFD860},
    {"shard", "Mirror Shard", "Reflection", Mirror, 0xD0D0D8},
    {"ink", "Raven Ink", "Night", Invert, 0x382F48},
}};

constexpr std::array<std::string_view, 12> Onsets = {"zar", "vel", "mor", "quin", "thal", "ish",
                                                       "dra", "bel", "nox", "fyr", "ul", "kor"};
constexpr std::array<std::string_view, 7> Middles = {"a", "o", "e", "i", "u", "ae", "ou"};
constexpr std::array<std::string_view, 9> Codas = {"th", "n", "x", "ra", "dor", "nis", "l", "mir", "oth"};

std::string Word(Dice &Luck, bool Capital)
{
    std::string Out;
    Out += Onsets[Luck.Below(Onsets.size())];
    Out += Middles[Luck.Below(Middles.size())];
    Out += Codas[Luck.Below(Codas.size())];
    if (Capital) Out[0] = static_cast<char>(Out[0] - 'a' + 'A');
    return Out;
}

// Three words nobody has said before, and the charm's own closing.
std::string Verse(const Charm &Which, Dice &Luck)
{
    std::string Out = Word(Luck, true);
    Out += ' ';
    Out += Word(Luck, false);
    Out += ' ';
    Out += Word(Luck, true);
    Out += ", ";
    Out += Which.Closings[Luck.Below(Which.Closings.size())];
    return Out;
}

std::uint8_t Clamped(std::uint32_t Value) noexcept
{
    return static_cast<std::uint8_t>(std::clamp<std::uint32_t>(Value, 1, 255));
}

constexpr std::array<std::string_view, 4> Toasts = {
    "Down the hatch! The room will not be the same.", "Bottoms up, and hold on to something!",
    "Glug, glug... ah, now it begins!", "Smells like a bad idea. Perfect."};

// The axes of what has a swing or a flip in it, and none of what has not.
std::uint8_t AxesOf(std::uint32_t Kinds, std::uint8_t Wanted) noexcept
{
    return (Kinds & (Sway | Mirror)) ? Wanted : 0;
}

}  // namespace

std::uint32_t Dice::Next() noexcept
{
    State ^= State << 13;
    State ^= State >> 17;
    State ^= State << 5;
    return State;
}

const std::array<Charm, CharmCount> &Charms() noexcept
{
    return g_charms;
}

const std::array<Ingredient, IngredientCount> &Ingredients() noexcept
{
    return g_ingredients;
}

const Charm *CharmNamed(std::string_view Word) noexcept
{
    for (const Charm &One : g_charms)
        if (One.Word == Word) return &One;
    return nullptr;
}

std::size_t IngredientNamed(std::string_view Word) noexcept
{
    for (std::size_t At = 0; At < g_ingredients.size(); At++)
        if (g_ingredients[At].Word == Word) return At;
    return NoIngredient;
}

Casting Spell(const Charm &Which, Dice &Luck, const Look &Shown)
{
    Casting Out;
    Out.Of = &Which;
    Out.Name = std::string(Which.Name);
    Out.Verse = Verse(Which, Luck);
    Out.Kinds = Which.Kinds & Shown.Allowed;
    if (Out.Kinds == 0) return Out;
    Out.Axes = AxesOf(Out.Kinds, Which.Axes);
    Out.Color = Which.Tint;
    Out.Strength = Clamped(Shown.StrengthPercent * 255u / 100u * (70u + Luck.Below(31)) / 100u);
    Out.DurationMs = Shown.DurationSeconds * 1000u * (80u + Luck.Below(21)) / 100u;
    return Out;
}

Casting Spell(Dice &Luck, const Look &Shown, const Charm *Last)
{
    std::array<const Charm *, CharmCount> Fit{};
    std::size_t Count = 0;
    for (const Charm &One : g_charms)
        if ((One.Kinds & Shown.Allowed) != 0 && &One != Last) Fit[Count++] = &One;
    if (Count == 0) {
        for (const Charm &One : g_charms)
            if ((One.Kinds & Shown.Allowed) != 0) Fit[Count++] = &One;
    }
    if (Count == 0) return Spell(g_charms[Luck.Below(g_charms.size())], Luck, Shown);
    return Spell(*Fit[Luck.Below(static_cast<std::uint32_t>(Count))], Luck, Shown);
}

std::uint32_t Liquid(std::span<const std::uint8_t> Mixed) noexcept
{
    if (Mixed.empty()) return 0;
    std::uint32_t Red = 0, Green = 0, Blue = 0;
    for (std::uint8_t One : Mixed) {
        const std::uint32_t Color = g_ingredients[One].Color;
        Red += (Color >> 16) & 0xFF;
        Green += (Color >> 8) & 0xFF;
        Blue += Color & 0xFF;
    }
    const auto Count = static_cast<std::uint32_t>(Mixed.size());
    return ((Red / Count) << 16) | ((Green / Count) << 8) | (Blue / Count);
}

Casting Potion(std::span<const std::uint8_t> Mixed, Dice &Luck, const Look &Shown)
{
    Casting Out;
    if (Mixed.empty()) return Out;

    std::array<std::uint32_t, IngredientCount> Amount{};
    std::uint32_t Kinds = 0;
    bool Doubled = false;
    std::string Spirits[CauldronMost];
    std::size_t Distinct = 0;
    for (std::uint8_t One : Mixed) {
        if (Amount[One]++ == 0) {
            Spirits[Distinct++] = std::string(g_ingredients[One].Spirit);
            Kinds |= g_ingredients[One].Kinds;
        } else {
            Doubled = true;
        }
    }

    const auto Count = static_cast<std::uint32_t>(Mixed.size());
    const std::uint32_t Base = Shown.StrengthPercent * 255u / 100u;
    const std::uint32_t Repeats = Count - static_cast<std::uint32_t>(Distinct);
    Out.Kinds = Kinds & Shown.Allowed;
    Out.Strength = Clamped(Base * (60u + 20u * Count) / 100u + Base * 10u * Repeats / 100u);
    Out.Axes = AxesOf(Out.Kinds, static_cast<std::uint8_t>(AxisX | (Doubled ? AxisY : 0)));
    Out.Color = Liquid(Mixed);
    Out.DurationMs = Shown.DurationSeconds * 1000u * (70u + 15u * Count) / 100u;

    Out.Verse = std::string(Toasts[Luck.Below(Toasts.size())]);
    Out.Name = Distinct == 1 ? "Potion of " : Distinct == 2 ? "Draught of " : "Elixir of ";
    for (std::size_t At = 0; At < Distinct; At++) {
        if (At > 0) Out.Name += At + 1 == Distinct ? " and " : ", ";
        Out.Name += Spirits[At];
    }
    return Out;
}

}  // namespace wizard
