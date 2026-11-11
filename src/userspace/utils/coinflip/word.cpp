/* word.cpp — the letters, each three dots across and five down. */

#include "word.h"

#include <cstddef>

namespace coinflip {

namespace {

constexpr std::int32_t LetterColumns = 3;

struct Letter {
    char         Name;
    std::uint8_t Rows[LetterRows];   // three bits each, the left dot the highest
};

constexpr Letter Letters[] = {
    {'H', {0b101, 0b101, 0b111, 0b101, 0b101}}, {'E', {0b111, 0b100, 0b111, 0b100, 0b111}},
    {'A', {0b010, 0b101, 0b111, 0b101, 0b101}}, {'D', {0b110, 0b101, 0b101, 0b101, 0b110}},
    {'S', {0b111, 0b100, 0b111, 0b001, 0b111}}, {'T', {0b111, 0b010, 0b010, 0b010, 0b010}},
    {'I', {0b111, 0b010, 0b010, 0b010, 0b111}}, {'L', {0b100, 0b100, 0b100, 0b100, 0b111}},
};

const Letter *Find(char Name)
{
    for (const Letter &One : Letters)
        if (One.Name == Name) return &One;
    return nullptr;
}

}  // namespace

std::int32_t WordWidth(std::string_view Word, std::int32_t Dot)
{
    const auto Count = static_cast<std::int32_t>(Word.size());
    return Count == 0 ? 0 : (Count * (LetterColumns + 1) - 1) * Dot;
}

void PaintWord(painter::tui::Frame &Out, std::int32_t X, std::int32_t Y, std::string_view Word, std::int32_t Dot,
               painter::core::Color Ink)
{
    std::int32_t Left = X;
    for (const char Name : Word) {
        if (const Letter *One = Find(Name)) {
            for (std::int32_t Row = 0; Row < LetterRows; Row++)
                for (std::int32_t Column = 0; Column < LetterColumns; Column++) {
                    if ((One->Rows[Row] & (1u << (LetterColumns - 1 - Column))) == 0) continue;
                    for (std::int32_t Down = 0; Down < Dot; Down++)
                        for (std::int32_t Across = 0; Across < Dot; Across++)
                            Out.PutPixel({Left + Column * Dot + Across, Y + Row * Dot + Down}, Ink);
                }
        }
        Left += (LetterColumns + 1) * Dot;
    }
}

}  // namespace coinflip
