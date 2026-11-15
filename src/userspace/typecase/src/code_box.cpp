/* code_box.cpp — the picture of a character no source has. */

#include "typecase/code_box.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace typecase {

namespace {

/* The hexadecimal digits, three pixels by five: a row's pixels in its low
 * three bits, the leftmost the highest. */
constexpr std::uint8_t Digits[16][5] = {
    {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7},
    {5, 5, 7, 1, 1}, {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 2, 2, 2},
    {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7}, {2, 5, 7, 5, 5}, {6, 5, 6, 5, 6},
    {3, 4, 4, 4, 3}, {6, 5, 5, 5, 6}, {7, 4, 7, 4, 7}, {7, 4, 7, 4, 4},
};

struct Layout {
    std::int32_t Columns;
    std::int32_t Rows;
};

/* The cell filled, as a picture against the pen on its baseline. */
GlyphImage Filled(const Size &At)
{
    GlyphImage Image;
    Image.Top     = -At.Baseline;
    Image.Width   = std::max(At.Width, 0);
    Image.Height  = std::max(At.Height, 0);
    Image.Advance = static_cast<float>(Image.Width);
    Image.Coverage.assign(static_cast<std::size_t>(Image.Width) * static_cast<std::size_t>(Image.Height), 255);
    return Image;
}

}  // namespace

GlyphImage DrawEmptyBox(const Size &At)
{
    GlyphImage         Image = Filled(At);
    const std::int32_t W = Image.Width, H = Image.Height;
    if (W < 3 || H < 3) return Image;
    const std::int32_t Line = std::max(1, (W + 5) / 10);
    for (std::int32_t Y = 0; Y < H; Y++)
        for (std::int32_t X = 0; X < W; X++) {
            const bool Inside = X >= 1 && X < W - 1 && Y >= 1 && Y < H - 1;
            const bool Edge   = X < 1 + Line || X >= W - 1 - Line || Y < 1 + Line || Y >= H - 1 - Line;
            Image.Coverage[static_cast<std::size_t>(Y * W + X)] = Inside && Edge ? 255 : 0;
        }
    return Image;
}

GlyphImage DrawCodeBox(char32_t Character, const Size &At)
{
    GlyphImage Image = Filled(At);

    const std::int32_t Count = Character <= 0xFFFF ? 4 : 6;
    const Layout       Four[] = {{2, 2}}, Six[] = {{2, 3}, {3, 2}};
    const Layout      *Tried = Count == 4 ? Four : Six;
    const std::size_t  Ways  = Count == 4 ? 1 : 2;

    /* The largest digits that fit, a row of ink kept above and below them. */
    Layout       Chosen{0, 0};
    std::int32_t Times = 0;
    for (std::int32_t Trying = std::min(Image.Width, Image.Height) / 4; Trying >= 1 && Times == 0; Trying--) {
        for (std::size_t Way = 0; Way < Ways; Way++) {
            const std::int32_t Wide = Tried[Way].Columns * 4 * Trying - Trying;
            const std::int32_t Tall = Tried[Way].Rows * 6 * Trying - Trying;
            if (Wide <= Image.Width && Tall + 2 * Trying <= Image.Height) {
                Chosen = Tried[Way];
                Times  = Trying;
                break;
            }
        }
    }
    if (Times == 0) return DrawEmptyBox(At);

    const std::int32_t Left = (Image.Width - (Chosen.Columns * 4 * Times - Times)) / 2;
    const std::int32_t Top  = (Image.Height - (Chosen.Rows * 6 * Times - Times)) / 2;
    for (std::int32_t Index = 0; Index < Count; Index++) {
        const std::uint32_t Digit = (static_cast<std::uint32_t>(Character) >> (4 * (Count - 1 - Index))) & 0xFu;
        const std::int32_t  X0    = Left + (Index % Chosen.Columns) * 4 * Times;
        const std::int32_t  Y0    = Top + (Index / Chosen.Columns) * 6 * Times;
        for (std::int32_t Y = 0; Y < 5 * Times; Y++)
            for (std::int32_t X = 0; X < 3 * Times; X++)
                if (Digits[Digit][Y / Times] & (4u >> (X / Times)))
                    Image.Coverage[static_cast<std::size_t>(Y0 + Y) * static_cast<std::size_t>(Image.Width) +
                                   static_cast<std::size_t>(X0 + X)] = 0;
    }
    return Image;
}

}  // namespace typecase
