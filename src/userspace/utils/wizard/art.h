/* art.h — the pictures: the room, the wizard, the cauldron and what rises from
 * it, drawn in pixels two to a cell high. A pixel of the picture is Scale by
 * Scale pixels of the frame, so the same wizard stands on a text page and on a
 * framebuffer. Only what the wizard does is here, and the clock it is drawn
 * by; what is said is paint.cpp's. */
#ifndef WIZARD_ART_H
#define WIZARD_ART_H

#include <cstdint>
#include <vector>

#include "painter/core/geometry.h"
#include "painter/tui/frame.h"

#include "look.h"

namespace wizard {

// Where the picture is drawn: its top left in pixels of the frame, how many
// frame pixels one of the picture is, and the picture's size in its own.
struct Pen {
    painter::tui::Frame *Out;
    painter::core::Point Origin;
    std::int32_t         Scale;
    painter::core::Size  Size;

    void Dot(std::int32_t X, std::int32_t Y, Color Ink) const;
    void Block(std::int32_t X, std::int32_t Y, std::int32_t Width, std::int32_t Height, Color Ink) const;
    // Ink over what is there, Weight of 255.
    void Veil(std::int32_t X, std::int32_t Y, Color Ink, std::uint8_t Weight) const;
};

// A speck that drifts and fades: a bubble, a puff of steam, a spark.
struct Speck {
    std::int32_t  X, Y;          // in sixteenths of a pixel of the picture
    std::int32_t  Rise, Drift;   // sixteenths a second
    std::uint32_t LifeMs, AgeMs;
    Color         Ink;
    bool          Glows;         // brighter than the room: sparks and bubbles, not steam
};

class Specks {
public:
    void Add(const Speck &One);
    void Step(std::uint32_t ElapsedMs);
    void Draw(const Pen &At) const;
    std::size_t Count() const noexcept { return All.size(); }

private:
    std::vector<Speck> All;
};

// The wizard stands at X, Y (his hat's tip's column, the floor's row less his
// height) and glows with Excitement from 0 to 255. Blinks and sways by Ms.
void DrawWizard(const Pen &At, std::int32_t X, std::int32_t Y, const Look &Shown, std::uint64_t Ms,
                std::uint32_t Excitement);

// The cauldron's top left at X, Y: its fire, its pot and its liquid, in Liquid
// (0: water), as full as Fullness of 4.
void DrawCauldron(const Pen &At, std::int32_t X, std::int32_t Y, std::uint32_t Liquid, std::uint32_t Fullness,
                  std::uint64_t Ms);

// The wall, the window with its moon and its stars, and the floor.
void DrawRoom(const Pen &At, const Look &Shown, std::uint64_t Ms);

// The sizes the pictures take, in pixels of the picture.
inline constexpr std::int32_t WizardWidth = 31;
inline constexpr std::int32_t WizardHeight = 34;
inline constexpr std::int32_t CauldronWidth = 22;
inline constexpr std::int32_t CauldronHeight = 19;
inline constexpr std::int32_t FloorHeight = 3;

// Where the staff's stone is, from the wizard's X, Y: sparks leave from it.
painter::core::Point StoneOf(std::int32_t X, std::int32_t Y) noexcept;
// Where the liquid's surface is, from the cauldron's X, Y: bubbles rise from it.
painter::core::Point SurfaceOf(std::int32_t X, std::int32_t Y) noexcept;

}  // namespace wizard

#endif
