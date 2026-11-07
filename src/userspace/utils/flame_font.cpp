/* flame_font.cpp — a word as lit pixels. A 5x7 font of its own: the lit
 * pixels are fire's sources, so the letters only have to be readable as a
 * shape, and a bigger letter is the same shape with fatter strokes. */

#include "flame.h"

namespace {

constexpr int GlyphWidth  = 5;
constexpr int GlyphHeight = 7;
constexpr int GlyphGap    = 1;

struct Glyph {
    char         Ch;
    std::uint8_t Rows[GlyphHeight];
};

constexpr Glyph Glyphs[] = {
    {' ', {0, 0, 0, 0, 0, 0, 0}},
    {'A', {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'B', {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110}},
    {'C', {0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110}},
    {'D', {0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110}},
    {'E', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111}},
    {'F', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'G', {0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111}},
    {'H', {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'I', {0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'J', {0b00111, 0b00010, 0b00010, 0b00010, 0b00010, 0b10010, 0b01100}},
    {'K', {0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001}},
    {'L', {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111}},
    {'M', {0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001}},
    {'N', {0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001, 0b10001}},
    {'O', {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'P', {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'Q', {0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101}},
    {'R', {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001}},
    {'S', {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110}},
    {'T', {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'U', {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'V', {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100}},
    {'W', {0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b10101, 0b01010}},
    {'X', {0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001}},
    {'Y', {0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'Z', {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111}},
    {'0', {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110}},
    {'1', {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'2', {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111}},
    {'3', {0b11110, 0b00001, 0b00001, 0b01110, 0b00001, 0b00001, 0b11110}},
    {'4', {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010}},
    {'5', {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110}},
    {'6', {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110}},
    {'7', {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000}},
    {'8', {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110}},
    {'9', {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100}},
    {'!', {0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00000, 0b00100}},
    {'?', {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b00000, 0b00100}},
    {'.', {0, 0, 0, 0, 0, 0b01100, 0b01100}},
    {',', {0, 0, 0, 0, 0b01100, 0b00100, 0b01000}},
    {'-', {0, 0, 0, 0b11111, 0, 0, 0}},
    {'+', {0, 0b00100, 0b00100, 0b11111, 0b00100, 0b00100, 0}},
    {':', {0, 0b01100, 0b01100, 0, 0b01100, 0b01100, 0}},
    {'\'', {0b00100, 0b00100, 0b01000, 0, 0, 0, 0}},
    {'/', {0b00001, 0b00010, 0b00010, 0b00100, 0b01000, 0b01000, 0b10000}},
    {'=', {0, 0, 0b11111, 0, 0b11111, 0, 0}},
    {'_', {0, 0, 0, 0, 0, 0, 0b11111}},
    {'*', {0, 0b00100, 0b10101, 0b01110, 0b10101, 0b00100, 0}},
    {'#', {0b01010, 0b01010, 0b11111, 0b01010, 0b11111, 0b01010, 0b01010}},
    {'(', {0b00010, 0b00100, 0b01000, 0b01000, 0b01000, 0b00100, 0b00010}},
    {')', {0b01000, 0b00100, 0b00010, 0b00010, 0b00010, 0b00100, 0b01000}},
};

const Glyph &GlyphOf(char Ch)
{
    if (Ch >= 'a' && Ch <= 'z') Ch = static_cast<char>(Ch - 'a' + 'A');
    const Glyph *Question = &Glyphs[0];
    for (const Glyph &Candidate : Glyphs) {
        if (Candidate.Ch == Ch) return Candidate;
        if (Candidate.Ch == '?') Question = &Candidate;
    }
    return *Question;
}

/* The biggest scale at which the word fits across and leaves the flames room
 * above: at most two fifths of the height, so the word is not all there is. */
int ScaleFor(int Letters, int Width, int Height)
{
    int Across = Letters * (GlyphWidth + GlyphGap) - GlyphGap;
    int Scale  = 1;
    while ((Scale + 1) * Across <= Width - 4 && (Scale + 1) * GlyphHeight * 5 <= Height * 2) Scale++;
    return Scale;
}

}

std::vector<std::uint8_t> FlameWordPixels(const std::string &Word, int Width, int Height)
{
    std::vector<std::uint8_t> Pixels(static_cast<std::size_t>(Width) * Height, 0);
    if (Word.empty()) return Pixels;

    int Letters = static_cast<int>(Word.size());
    int Scale   = ScaleFor(Letters, Width, Height);
    int Across  = (Letters * (GlyphWidth + GlyphGap) - GlyphGap) * Scale;
    int Left    = (Width - Across) / 2;
    int Top     = Height * 3 / 5 - GlyphHeight * Scale / 2;

    for (int Letter = 0; Letter < Letters; Letter++) {
        const Glyph &Shape = GlyphOf(Word[static_cast<std::size_t>(Letter)]);
        int          Start = Left + Letter * (GlyphWidth + GlyphGap) * Scale;
        for (int Row = 0; Row < GlyphHeight * Scale; Row++) {
            int Y = Top + Row;
            if (Y < 0 || Y >= Height) continue;
            std::uint8_t Bits = Shape.Rows[Row / Scale];
            for (int Col = 0; Col < GlyphWidth * Scale; Col++) {
                int X = Start + Col;
                if (X < 0 || X >= Width) continue;
                if (Bits & (1u << (GlyphWidth - 1 - Col / Scale)))
                    Pixels[static_cast<std::size_t>(Y) * Width + X] = 1;
            }
        }
    }
    return Pixels;
}
