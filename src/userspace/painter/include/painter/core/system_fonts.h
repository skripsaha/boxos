#ifndef PAINTER_CORE_SYSTEM_FONTS_H
#define PAINTER_CORE_SYSTEM_FONTS_H

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "typecase/fonts.h"

namespace painter::core {

// The system's fonts: every file on the volume that begins as a font this
// typesetter reads (typecase::FontBeginnings) and reads as one, gathered into
// one book (typecase/fonts.h) in the order the volume made them — its default
// family the system's text, Inter (SystemTextFamily), every other family a
// fallback.
// Read the first time they are asked, and kept: text a program draws itself
// is set in them unless it says otherwise. A file that will not read is said
// and passed over. The cells of the console keep their own font, the
// display's setting "text font": a family for code, not for text.
std::shared_ptr<const typecase::Fonts> SystemFonts();

// The family text is set in when nothing says another — until the theme
// names the family of each role.
inline constexpr const char *SystemTextFamily = "Inter";

// The font file called Name — and wearing every tag of the person's Tags
// besides (apart by commas, "" for none: a program that names some asks
// `see-tags`) — found anywhere on the volume, not only in the Use Context, and
// read whole: the bytes, nothing made of them. Of two files that answer, the
// first made is read, and that is said. No file, or one that will not read:
// the sentence that says why.
std::expected<std::vector<std::uint8_t>, std::string> ReadFontFile(std::string_view Name, std::string_view Tags = {});

}  // namespace painter::core

#endif  // PAINTER_CORE_SYSTEM_FONTS_H
