#ifndef BOXOS_DISPLAY_FONT_FILE_H
#define BOXOS_DISPLAY_FONT_FILE_H

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

// The font file the person named — called Name, and wearing every tag of
// Tags besides (apart by commas, "" for none) — found anywhere on the volume,
// not only in the Use Context, and read whole: the bytes, nothing made of
// them. Of two files that answer, the first made is read, and that is said.
// No file, or one that will not read: the sentence that says why.
std::expected<std::vector<std::uint8_t>, std::string> FontFileRead(std::string_view Name, std::string_view Tags);

#endif
