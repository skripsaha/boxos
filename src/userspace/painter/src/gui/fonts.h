#ifndef PAINTER_GUI_FONTS_H
#define PAINTER_GUI_FONTS_H

// The program's fonts as the display keeps them for it (display/areas.cpp
// BookOf): the system's faces first, then each font an area of the program
// opened — so a text measured here is laid in the faces the display lays it in.

#include <cstdint>
#include <span>
#include <utility>

#include "typecase/fonts.h"

namespace painter::gui::detail {

// A font an area of the program opened, read into its fonts after the
// system's.
void ProgramFontsAdd(std::span<const std::uint8_t> Bytes);

const typecase::Fonts &ProgramFontsLocked();
void                   ProgramFontsLock() noexcept;
void                   ProgramFontsUnlock() noexcept;

// Lay laid in the program's fonts, under their lock. Throws what Lay throws.
template <typename Laying>
auto WithProgramFonts(Laying &&Lay) -> decltype(Lay(std::declval<const typecase::Fonts &>()))
{
    struct Held {
        Held() noexcept { ProgramFontsLock(); }
        ~Held() { ProgramFontsUnlock(); }
    } Holding;
    return Lay(ProgramFontsLocked());
}

}  // namespace painter::gui::detail

#endif
