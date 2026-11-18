/* script.cpp — the writing a character belongs to. */

#include "typecase/script.h"

#include <cstddef>

namespace typecase {

namespace {

struct Range {
    char32_t First;
    char32_t Last;
    Script   Of;
};

/* Ascending and apart; what falls between them is Other. */
constexpr Range Ranges[] = {
    {0x0000, 0x0040, Script::Common},   {0x0041, 0x005A, Script::Latin},    {0x005B, 0x0060, Script::Common},
    {0x0061, 0x007A, Script::Latin},    {0x007B, 0x00A9, Script::Common},   {0x00AA, 0x00AA, Script::Latin},
    {0x00AB, 0x00B9, Script::Common},   {0x00BA, 0x00BA, Script::Latin},    {0x00BB, 0x00BF, Script::Common},
    {0x00C0, 0x00D6, Script::Latin},    {0x00D7, 0x00D7, Script::Common},   {0x00D8, 0x00F6, Script::Latin},
    {0x00F7, 0x00F7, Script::Common},   {0x00F8, 0x02AF, Script::Latin},    {0x02B0, 0x036F, Script::Common},
    {0x0370, 0x03FF, Script::Greek},    {0x0400, 0x052F, Script::Cyrillic}, {0x1C80, 0x1C8F, Script::Cyrillic},
    {0x1D00, 0x1D7F, Script::Latin},    {0x1DC0, 0x1DFF, Script::Common},   {0x1E00, 0x1EFF, Script::Latin},
    {0x1F00, 0x1FFF, Script::Greek},    {0x2000, 0x2BFF, Script::Common},   {0x2C60, 0x2C7F, Script::Latin},
    {0x2DE0, 0x2DFF, Script::Cyrillic}, {0x2E00, 0x2E7F, Script::Common},   {0x3000, 0x303F, Script::Common},
    {0xA640, 0xA69F, Script::Cyrillic}, {0xA720, 0xA7FF, Script::Latin},    {0xAB30, 0xAB6F, Script::Latin},
    {0xFB00, 0xFB06, Script::Latin},    {0xFE00, 0xFE0F, Script::Common},   {0xFE20, 0xFE2F, Script::Common},
    {0xFE30, 0xFE6F, Script::Common},   {0xFEFF, 0xFEFF, Script::Common},   {0xFF00, 0xFF20, Script::Common},
    {0xFF21, 0xFF3A, Script::Latin},    {0xFF3B, 0xFF40, Script::Common},   {0xFF41, 0xFF5A, Script::Latin},
    {0xFF5B, 0xFF65, Script::Common},   {0xFFF0, 0xFFFF, Script::Common},   {0x1F000, 0x1FAFF, Script::Common},
    {0xE0000, 0xE007F, Script::Common}, {0xE0100, 0xE01EF, Script::Common},
};

}  // namespace

Script ScriptOf(char32_t Character) noexcept
{
    std::size_t Low = 0, High = sizeof(Ranges) / sizeof(Ranges[0]);
    while (Low < High) {
        const std::size_t Middle = (Low + High) / 2;
        if (Ranges[Middle].Last < Character) Low = Middle + 1;
        else High = Middle;
    }
    if (Low < sizeof(Ranges) / sizeof(Ranges[0]) && Ranges[Low].First <= Character) return Ranges[Low].Of;
    return Script::Other;
}

}  // namespace typecase
