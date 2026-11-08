/* The looks a page can have. */

#include "theme.h"

namespace read {

namespace {

constexpr core::Color Rgb(std::uint32_t Hex) noexcept
{
    return core::Color::Rgb(static_cast<std::uint8_t>(Hex >> 16), static_cast<std::uint8_t>(Hex >> 8),
                            static_cast<std::uint8_t>(Hex));
}

constexpr Theme All[] = {
    {"dark",  Rgb(0x181818), Rgb(0xD8D4CC), Rgb(0x8A857C), Rgb(0xF2C46F), Rgb(0xC9A86A), Rgb(0xFFFFFF),
     Rgb(0xA8C7E8), Rgb(0xB8D4A0), Rgb(0x232323), Rgb(0x7FB8E6), Rgb(0x272A30), Rgb(0x2C4A6E), Rgb(0x5A4400),
     Rgb(0xFFC832), Rgb(0x141414), Rgb(0x202020), Rgb(0xA8A39A), Rgb(0xF2C46F), Rgb(0x262626), Rgb(0x9A9A9A),
     Rgb(0x2E4A6A), Rgb(0xFF6E6E)},
    {"sepia", Rgb(0xF1E7D0), Rgb(0x4A3B2A), Rgb(0x8C7A63), Rgb(0x8A3B12), Rgb(0x9C5A2A), Rgb(0x2A1D10),
     Rgb(0x6A4A8A), Rgb(0x3E5A2A), Rgb(0xE6DABB), Rgb(0x2A5A8A), Rgb(0xE6D8B4), Rgb(0xD6C08E), Rgb(0xF0C860),
     Rgb(0xC06020), Rgb(0xFFF8EA), Rgb(0xE4D6B4), Rgb(0x5A4A36), Rgb(0xA04818), Rgb(0xECE0C4), Rgb(0x8C7A63),
     Rgb(0xD6C08E), Rgb(0xB02020)},
    {"light", Rgb(0xFBFBF8), Rgb(0x262626), Rgb(0x7A7A7A), Rgb(0x1A4F9C), Rgb(0x2A6AAA), Rgb(0x000000),
     Rgb(0x7A3A9A), Rgb(0x2A6A2A), Rgb(0xEFEFEA), Rgb(0x1A5FB4), Rgb(0xEAF0F8), Rgb(0xC8DCF4), Rgb(0xFFE680),
     Rgb(0xF0A020), Rgb(0x000000), Rgb(0xECECEA), Rgb(0x4A4A4A), Rgb(0x1A4F9C), Rgb(0xF4F4F0), Rgb(0x8A8A8A),
     Rgb(0xC8DCF4), Rgb(0xC02020)},
};

}  // namespace

std::span<const Theme> Themes() noexcept
{
    return All;
}

const Theme &ThemeNamed(std::string_view Name) noexcept
{
    for (const Theme &One : All)
        if (One.Name == Name) return One;
    return All[0];
}

const Theme &ThemeAfter(const Theme &One) noexcept
{
    const std::size_t Index = static_cast<std::size_t>(&One - All);
    return All[(Index + 1) % (sizeof(All) / sizeof(All[0]))];
}

}  // namespace read
