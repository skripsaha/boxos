/* lettering.cpp — the picture of each capital letter (lettering.h). */

#include "lettering.h"

#include <array>

namespace hangman {

namespace {

// A letter's rows from the top, one bit a pixel, the leftmost the highest of the five.
using Rows = std::array<std::uint8_t, LetterHigh>;

constexpr std::array<Rows, 26> Capitals = {{
    {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001},   // A
    {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110},   // B
    {0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110},   // C
    {0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110},   // D
    {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111},   // E
    {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000},   // F
    {0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111},   // G
    {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001},   // H
    {0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110},   // I
    {0b00111, 0b00010, 0b00010, 0b00010, 0b00010, 0b10010, 0b01100},   // J
    {0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001},   // K
    {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111},   // L
    {0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001},   // M
    {0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001, 0b10001},   // N
    {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110},   // O
    {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000},   // P
    {0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101},   // Q
    {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001},   // R
    {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110},   // S
    {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100},   // T
    {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110},   // U
    {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100},   // V
    {0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b11011, 0b10001},   // W
    {0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001},   // X
    {0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100},   // Y
    {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111},   // Z
}};

}  // namespace

void PaintLetter(painter::tui::Frame &Out, char Letter, painter::core::Point Corner, std::int32_t Visible, painter::core::Color Ink)
{
    if (Letter < 'A' || Letter > 'Z') return;
    const Rows &Drawn = Capitals[static_cast<std::size_t>(Letter - 'A')];
    for (std::int32_t Row = LetterHigh - Visible; Row < LetterHigh; Row++)
        for (std::int32_t Column = 0; Column < LetterWide; Column++)
            if ((Drawn[static_cast<std::size_t>(Row)] >> (LetterWide - 1 - Column)) & 1)
                Out.PutPixel({Corner.X + Column, Corner.Y + Row}, Ink);
}

}  // namespace hangman
