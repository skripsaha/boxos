// boxcxx — box::glass  (an effect over everything the display shows)
//
// The C++ face of display_effect and display_effect_lift (box/display.h). Any
// program may ask the display to lay one effect over the whole glass for a
// while — its text, every layer, the caret — and the display ends it at its
// time, when the asking program ends, or when it is lifted. One at a time: a
// new effect takes the place of the one laid, whoever laid it.
//
//   box::glass::lay({.kinds = DISPLAY_EFFECT_SWAY | DISPLAY_EFFECT_TINT,
//                    .strength = 160, .axes = DISPLAY_EFFECT_AXIS_X | DISPLAY_EFFECT_AXIS_Y,
//                    .color = 0x60D8F0, .duration = 20s})
//   box::glass::lift()                — the effect lifted: true when one was
//
// This is a box:: extension, not part of std.
#ifndef BOXCXX_BOX_GLASS_H
#define BOXCXX_BOX_GLASS_H

#include <chrono>
#include <cstdint>

#include "box/cxx/error.h"  // box::result / box::status
#include "box/display.h"

namespace box {

// What is laid: DISPLAY_EFFECT_* kinds together, one strength for them (1..255),
// DISPLAY_EFFECT_AXIS_* for a sway and a mirror, the tint's #RRGGBB, and how long.
struct glass_effect {
    std::uint32_t             kinds    = 0;
    std::uint8_t              strength = 0;
    std::uint8_t              axes     = 0;
    std::uint32_t             color    = 0;
    std::chrono::milliseconds duration{};
};

struct glass {
    // The effect laid over the glass, in place of any other.
    static status lay(const glass_effect &effect) noexcept
    {
        const std::int64_t ms = effect.duration.count() < 0 ? 0 : effect.duration.count();
        const int rc = ::display_effect(effect.kinds, effect.strength, effect.axes, effect.color,
                                        ms > DISPLAY_EFFECT_LONGEST_MS ? DISPLAY_EFFECT_LONGEST_MS
                                                                       : static_cast<std::uint32_t>(ms));
        if (rc != OK) return std::unexpected(error{box_errno_of(rc)});
        return {};
    }

    // The effect lifted, if it is this program's (or this program is a keeper):
    // true when one was lifted, false when there was none.
    static result<bool> lift() noexcept
    {
        const int rc = ::display_effect_lift();
        if (rc < 0) return std::unexpected(error{box_errno_of(rc)});
        return rc == 1;
    }
};

}  // namespace box

#endif  // BOXCXX_BOX_GLASS_H
