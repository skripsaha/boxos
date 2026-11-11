/* lore.cpp — the charms, the things in the cauldron, the potions and the words. */

#include "lore.h"

#include <algorithm>
#include <format>

#include "box/display.h"

namespace wizard {

namespace {

constexpr std::uint32_t Sway = DISPLAY_EFFECT_SWAY;
constexpr std::uint32_t Mirror = DISPLAY_EFFECT_MIRROR;
constexpr std::uint32_t Fade = DISPLAY_EFFECT_FADE;
constexpr std::uint32_t Invert = DISPLAY_EFFECT_INVERT;
constexpr std::uint32_t Tint = DISPLAY_EFFECT_TINT;
constexpr std::uint32_t Shimmer = DISPLAY_EFFECT_SHIMMER;
constexpr std::uint32_t Ripple = DISPLAY_EFFECT_RIPPLE;
constexpr std::uint32_t Quake = DISPLAY_EFFECT_QUAKE;
constexpr std::uint32_t Mosaic = DISPLAY_EFFECT_MOSAIC;
constexpr std::uint32_t Rainbow = DISPLAY_EFFECT_RAINBOW;
constexpr std::uint32_t Melt = DISPLAY_EFFECT_MELT;
constexpr std::uint32_t Glitch = DISPLAY_EFFECT_GLITCH;
constexpr std::uint32_t Spotlight = DISPLAY_EFFECT_SPOTLIGHT;
constexpr std::uint32_t Sepia = DISPLAY_EFFECT_SEPIA;
constexpr std::uint32_t Snow = DISPLAY_EFFECT_SNOW;
constexpr std::uint32_t Pulse = DISPLAY_EFFECT_PULSE;
constexpr std::uint8_t  AxisX = DISPLAY_EFFECT_AXIS_X;
constexpr std::uint8_t  AxisY = DISPLAY_EFFECT_AXIS_Y;
constexpr std::uint32_t Moves = Sway | Mirror | Quake;     // the kinds that want an axis

const std::array<Charm, CharmCount> g_charms = {{
    {"giddy", "Giddy Sway", Sway | Tint, AxisX | AxisY, 0xB8E070, Mood::Dizzy,
     {"let the floor forget which way is down!", "let every line be a little tipsy!"}},
    {"mirror", "Looking-Glass", Mirror, AxisX, 0xFFFFFF, Mood::Surprised,
     {"show the world its other face!", "let right be left for a while!"}},
    {"upside", "Upside-Down Tuesday", Mirror, AxisY, 0xFFFFFF, Mood::Surprised,
     {"turn the ceiling into a floor!", "let the sky stand on its head!"}},
    {"ghost", "Ghost Colors", Invert, 0, 0xFFFFFF, Mood::Spooked,
     {"let the dark be light and the light be dark!", "paint the night in the colors of day!"}},
    {"grey", "Grey Monday", Fade, 0, 0xFFFFFF, Mood::Pleased,
     {"drain the color from the day!", "let every hue go quiet!"}},
    {"ember", "Ember Glass", Tint | Shimmer, 0, 0xFF7030, Mood::Pleased,
     {"set the air alight without a flame!", "warm the glass with a dragon's breath!"}},
    {"frost", "Frostbite", Tint | Snow, 0, 0x60D8F0, Mood::Pleased,
     {"breathe winter onto the glass!", "let the cold paint every word!"}},
    {"starfall", "Starfall", Shimmer | Snow, 0, 0xFFF0B0, Mood::Pleased,
     {"rain the stars down on the room!", "let the night spill its sparkles!"}},
    {"moonlit", "Moonlit", Tint | Fade, 0, 0xB8C4FF, Mood::Pleased,
     {"let the moon look in at the window!", "wash it all in silver!"}},
    {"fever", "Fever Dream", Sway | Tint | Shimmer, AxisX, 0xC070FF, Mood::Dizzy,
     {"let the room dream with its eyes open!", "stir the air like a spoon in a dream!"}},
    {"pond", "Pond Ripples", Ripple | Tint, 0, 0x90E0E8, Mood::Surprised,
     {"drop a pebble into the middle of the world!", "let the glass remember water!"}},
    {"tremor", "Little Tremor", Quake, AxisX | AxisY, 0xFFFFFF, Mood::Surprised,
     {"let the mountain turn over in its sleep!", "shake the dust out of the sky!"}},
    {"pixie", "Pixie Pixels", Mosaic | Shimmer, 0, 0xFFE8A0, Mood::Squinting,
     {"break the picture into little tiles!", "let the world be counted in squares!"}},
    {"prism", "Prism", Rainbow, 0, 0xFFFFFF, Mood::Pleased,
     {"let the light come apart into its colors!", "hang a rainbow over everything!"}},
    {"drip", "Melting Hour", Melt | Tint, 0, 0xFFC878, Mood::Surprised,
     {"let the afternoon run down the wall!", "let the picture drip like candle wax!"}},
    {"hex", "Hexed Wires", Glitch, 0, 0xFFFFFF, Mood::Spooked,
     {"tear a seam in the weave of things!", "let the air stutter and forget its line!"}},
    {"lantern", "Lantern Night", Spotlight, 0, 0xFFFFFF, Mood::Squinting,
     {"leave only one small light in the dark!", "send a lantern wandering over the world!"}},
    {"yore", "Old Reel", Sepia, 0, 0xFFFFFF, Mood::Squinting,
     {"turn the day into a flickering memory!", "let it all be a film from long ago!"}},
    {"blizzard", "Blizzard", Snow | Tint | Fade, 0, 0xDDEEFF, Mood::Pleased,
     {"send the north wind in at the door!", "let it snow where it never snowed!"}},
    {"heartbeat", "Heartbeat", Pulse | Tint, 0, 0xFF6070, Mood::Surprised,
     {"let the room remember it is alive!", "make the walls breathe in and out!"}},
    {"carnival", "Carnival", Rainbow | Pulse | Shimmer, 0, 0xFFFFFF, Mood::Dizzy,
     {"let the fair come to town tonight!", "let every light dance on its own!"}},
    {"nightmare", "Nightmare Static", Spotlight | Glitch | Invert, 0, 0xFFFFFF, Mood::Spooked,
     {"let the dark have a look back at you!", "wake what sleeps in the wires!"}},
    {"tide", "Tidal Dream", Ripple | Sway | Tint, AxisX, 0x70A8F0, Mood::Dizzy,
     {"let the sea remember where the floor was!", "drown the room in a gentle swell!"}},
    {"avalanche", "White Avalanche", Quake | Snow | Fade, AxisY, 0xFFFFFF, Mood::Surprised,
     {"let the mountain come down in white!", "shake the snow off every roof!"}},
}};

const std::array<Ingredient, IngredientCount> g_ingredients = {{
    {"mandrake", "Mandrake Root", "Tremors", "a root shaped like a little man that screams when pulled", Quake, 0x8A6A3A},
    {"toadstool", "Toadstool", "Giddiness", "a red cap with white spots, and a stem that sways", Sway, 0xC04838},
    {"feather", "Raven Feather", "Night", "black as a keyhole, and as watchful", Spotlight, 0x4A3C70},
    {"moonstone", "Moonstone", "Pallor", "a pale stone that has drunk the moon's light", Fade, 0xC8D4E8},
    {"scale", "Dragon Scale", "Fire", "still warm, however long ago it fell", Tint | Shimmer, 0xE0502A},
    {"eye", "Frog Eye", "Ripples", "it watches the pond, and the pond watches back", Ripple, 0x58B070},
    {"stardust", "Stardust", "Stars", "swept from the roof on a clear night", Shimmer | Snow, 0xFFD860},
    {"tail", "Salamander Tail", "Melting", "it grows back, and it is never dry", Melt, 0xE88A30},
    {"fern", "Moon Fern", "Growth", "it unrolls a little more with every breath", Pulse, 0x3EA050},
    {"bone", "Old Bone", "Haunting", "somebody's, once; he does not say whose", Invert, 0xE4DCC8},
    {"berry", "Frost Berry", "Frost", "cold enough to burn the fingers", Tint | Snow, 0x40C0D8},
    {"shard", "Mirror Shard", "Reflection", "it shows what is behind you", Mirror, 0xB8C4D8},
    {"moss", "Glow Moss", "Wonder", "it lights the cave and nobody knows how", Rainbow, 0x60E070},
    {"beetle", "Beetle Shell", "Pieces", "cut like a tile, and laid like a mosaic", Mosaic, 0x2E8A82},
    {"cobweb", "Cobweb", "Static", "it hums when nobody is in the room", Glitch, 0xC8C8D0},
    {"parchment", "Old Parchment", "Yesterday", "it smells of a library nobody remembers", Sepia, 0xD8B878},
}};

constexpr std::uint8_t Mandrake = 0, Toadstool = 1, Feather = 2, Moonstone = 3, Scale = 4, Eye = 5, Stardust = 6,
                       Tail = 7, Fern = 8, Bone = 9, Berry = 10, Shard = 11, Moss = 12, Beetle = 13, Cobweb = 14,
                       Parchment = 15;

const std::array<Recipe, RecipeCount> g_recipes = {{
    {"dizzy", "Draught of Dizzy Rainbows", {Toadstool, Moss, NoThing}, Pulse, 0xE070C0,
     "something that sways and something that glows", "The whole room spins in colors!"},
    {"snowfall", "Elixir of First Snow", {Stardust, Berry, NoThing}, Fade, 0xE8F4FF,
     "a cold thing and a thing from the sky", "Winter comes in through the window!"},
    {"haunted", "Potion of the Haunted Hour", {Feather, Bone, NoThing}, Glitch | Fade, 0x6A5A8C,
     "something black and something dead", "Do you hear that? Neither do I."},
    {"molten", "Draught of Molten Dreams", {Scale, Tail, NoThing}, Shimmer | Tint, 0xFF7A20,
     "two things that have met a fire", "Everything runs like warm honey."},
    {"stillwater", "Elixir of Still Water", {Eye, Moonstone, NoThing}, Tint | Fade, 0x80D0E0,
     "something that watches water and something pale", "Breathe in. The pond is calm."},
    {"rumble", "Potion of Rumbling Static", {Mandrake, Cobweb, NoThing}, Fade, 0xA09070,
     "a screaming root and something that hums", "The earth is on the line, and it is cross."},
    {"silent", "Draught of the Silent Film", {Parchment, Moonstone, NoThing}, Fade | Quake, 0xD0C0A0,
     "something old and something pale", "No words, only flickers."},
    {"garden", "Elixir of the Garden Party", {Fern, Moss, Stardust}, Tint, 0x70E090,
     "three things that grow and glow and fall", "Everyone is invited, even the beetles!"},
    {"broken", "Draught of Broken Mirrors", {Shard, Beetle, NoThing}, Glitch, 0x90C8C8,
     "something that reflects and something in pieces", "Seven years of luck, cut into tiles."},
    {"seasons", "Potion of Odd Seasons", {Toadstool, Berry, Scale}, Mosaic | Pulse, 0xE0A060,
     "something giddy, something cold, something hot", "Spring, winter and summer, all before lunch."},
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

// The axes of what has a swing, a flip or a jolt in it, and none of what has not.
std::uint8_t AxesOf(std::uint32_t Kinds, std::uint8_t Wanted) noexcept
{
    return (Kinds & Moves) ? Wanted : 0;
}

// What a mix of kinds looks like to him.
Mood FaceOf(std::uint32_t Kinds) noexcept
{
    if (Kinds & (Glitch | Spotlight | Invert)) return Mood::Spooked;
    if (Kinds & (Sway | Rainbow | Pulse)) return Mood::Dizzy;
    if (Kinds & (Quake | Ripple | Melt | Mirror)) return Mood::Surprised;
    if (Kinds & (Sepia | Mosaic)) return Mood::Squinting;
    return Mood::Pleased;
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

const std::array<Recipe, RecipeCount> &Recipes() noexcept
{
    return g_recipes;
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
    Out.Face = Which.Face;
    if (Out.Kinds == 0) return Out;
    Out.Axes = AxesOf(Out.Kinds, Which.Axes);
    Out.Color = Which.Tint;
    Out.Strength = Clamped(Shown.StrengthPercent * 255u / 100u * (70u + Luck.Below(31)) / 100u);
    Out.DurationMs = Shown.DurationSeconds * 1000u * (80u + Luck.Below(21)) / 100u;
    return Out;
}

const Charm &ChooseCharm(Dice &Luck, const Look &Shown, const Charm *Last)
{
    std::array<const Charm *, CharmCount> Fit{};
    std::size_t Count = 0;
    for (const Charm &One : g_charms)
        if ((One.Kinds & Shown.Allowed) != 0 && &One != Last) Fit[Count++] = &One;
    if (Count == 0) {
        for (const Charm &One : g_charms)
            if ((One.Kinds & Shown.Allowed) != 0) Fit[Count++] = &One;
    }
    if (Count == 0) return g_charms[Luck.Below(g_charms.size())];
    return *Fit[Luck.Below(static_cast<std::uint32_t>(Count))];
}

std::uint32_t Liquid(std::span<const std::uint8_t> Mixed) noexcept
{
    if (Mixed.empty()) return 0;
    if (const std::size_t Found = RecipeOf(Mixed); Found != NoIngredient) return g_recipes[Found].Color;
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

std::size_t RecipeOf(std::span<const std::uint8_t> Mixed) noexcept
{
    for (std::size_t At = 0; At < g_recipes.size(); At++) {
        const Recipe &One = g_recipes[At];
        std::size_t   Needs = 0;
        bool          Whole = true;
        for (std::uint8_t Part : One.Of) {
            if (Part == NoThing) continue;
            Needs++;
            Whole = Whole && std::find(Mixed.begin(), Mixed.end(), Part) != Mixed.end();
        }
        const bool OnlyThose = std::all_of(Mixed.begin(), Mixed.end(), [&](std::uint8_t Put) {
            return std::find(One.Of.begin(), One.Of.end(), Put) != One.Of.end();
        });
        if (Whole && OnlyThose && Needs > 0) return At;
    }
    return NoIngredient;
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

    const std::size_t Found = RecipeOf(Mixed);
    if (Found != NoIngredient) Kinds |= g_recipes[Found].Kinds;
    const auto Count = static_cast<std::uint32_t>(Mixed.size());
    const std::uint32_t Base = Shown.StrengthPercent * 255u / 100u;
    const std::uint32_t Repeats = Count - static_cast<std::uint32_t>(Distinct);
    Out.Found = Found;
    Out.Kinds = Kinds & Shown.Allowed;
    Out.Strength = Clamped(Base * (60u + 20u * Count) / 100u + Base * 10u * Repeats / 100u);
    Out.Axes = AxesOf(Out.Kinds, static_cast<std::uint8_t>(AxisX | (Doubled || Found != NoIngredient ? AxisY : 0)));
    Out.Color = Liquid(Mixed);
    Out.DurationMs = Shown.DurationSeconds * 1000u * (70u + 15u * Count) / 100u;
    Out.Face = FaceOf(Out.Kinds);

    if (Found != NoIngredient) {
        Out.Name = std::string(g_recipes[Found].Name);
        Out.Verse = std::string(g_recipes[Found].Verse);
        return Out;
    }
    Out.Verse = std::string(Toasts[Luck.Below(Toasts.size())]);
    Out.Name = Distinct == 1 ? "Potion of " : Distinct == 2 ? "Draught of " : "Elixir of ";
    for (std::size_t At = 0; At < Distinct; At++) {
        if (At > 0) Out.Name += At + 1 == Distinct ? " and " : ", ";
        Out.Name += Spirits[At];
    }
    return Out;
}

std::string EffectNames(std::uint32_t Kinds)
{
    static constexpr std::array<std::pair<std::uint32_t, std::string_view>, 16> Names = {{
        {DISPLAY_EFFECT_SWAY, "sway"}, {DISPLAY_EFFECT_MIRROR, "mirror"}, {DISPLAY_EFFECT_FADE, "fade"}, {DISPLAY_EFFECT_INVERT, "invert"},
        {DISPLAY_EFFECT_TINT, "tint"}, {DISPLAY_EFFECT_SHIMMER, "shimmer"}, {DISPLAY_EFFECT_RIPPLE, "ripple"}, {DISPLAY_EFFECT_QUAKE, "quake"},
        {DISPLAY_EFFECT_MOSAIC, "mosaic"}, {DISPLAY_EFFECT_RAINBOW, "rainbow"}, {DISPLAY_EFFECT_MELT, "melt"}, {DISPLAY_EFFECT_GLITCH, "glitch"},
        {DISPLAY_EFFECT_SPOTLIGHT, "spotlight"}, {DISPLAY_EFFECT_SEPIA, "sepia"}, {DISPLAY_EFFECT_SNOW, "snow"}, {DISPLAY_EFFECT_PULSE, "pulse"}}};
    std::string Out;
    for (const auto &[Bit, Name] : Names) {
        if (!(Kinds & Bit)) continue;
        if (!Out.empty()) Out += ", ";
        Out += Name;
    }
    return Out.empty() ? "nothing" : Out;
}

std::string Remark(std::size_t Which, std::span<const std::uint8_t> Mixed, Dice &Luck)
{
    const Ingredient &Put = g_ingredients[Which];
    if (Mixed.size() >= CauldronMost) return std::format("{}! That is the last of the room: the brew is ready, drink it!", Put.Name);
    if (const std::size_t Found = RecipeOf(Mixed); Found != NoIngredient)
        return std::format("{} goes in... and the brew goes still. This smells like something I have not made before!", Put.Name);
    switch (Luck.Below(4)) {
    case 0:  return std::format("{} - {}. Into the pot with it!", Put.Name, Put.Looks);
    case 1:  return std::format("Ah, {}: {}.", Put.Name, Put.Looks);
    case 2:  return std::format("{}, you say? {}. Splash!", Put.Name, Put.Looks);
    default: return std::format("In goes the {}: {}.", Put.Name, Put.Looks);
    }
}

}  // namespace wizard
