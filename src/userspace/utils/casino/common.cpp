/* common.cpp — what the tables say or draw alike (common.h). */

#include "common.h"

#include <format>

namespace casino {

std::string Counted(std::int64_t Chips)
{
    std::string Digits = std::to_string(Chips < 0 ? -Chips : Chips);
    for (std::ptrdiff_t At = static_cast<std::ptrdiff_t>(Digits.size()) - 3; At > 0; At -= 3) Digits.insert(static_cast<std::size_t>(At), ",");
    return Chips < 0 ? "-" + Digits : Digits;
}

std::string Spelled(const KeyChord &Chord)
{
    char Spelling[KEY_CHORD_SPELLED_MAX];
    KeyChordSpell(&Chord, Spelling, sizeof(Spelling));
    return Spelling;
}

Color HueColor(const Look &Shown, Hue Which)
{
    static const Color Green = Color::Rgb(0x1E, 0x9A, 0x4A);
    return Which == Hue::Red ? Shown.Red : Which == Hue::Black ? Shown.Black : Green;
}

std::string Entry(std::string_view Keys, std::string_view What)
{
    return std::format("{:<18}{}", Keys, What);
}

void Bulb(Canvas &Draw, float Across, float Down, Color Lights, bool Lit)
{
    if (Lit) {
        Draw.Disc(Across, Down, 1.7f, Shade(Lights, 0.45f));
        Draw.Disc(Across, Down, 1.0f, Shade(Lights, 1.35f));
    } else {
        Draw.Disc(Across, Down, 1.0f, Shade(Lights, 0.3f));
    }
}

}  // namespace casino
