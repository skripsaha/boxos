/* canvas_text.cpp — big letters for the tables: a font of five by seven
 * pixels, drawn at whole-number sizes (canvas.h). */

#include "canvas.h"

namespace casino {

namespace {

constexpr std::int32_t GlyphWide = 5;
constexpr std::int32_t GlyphHigh = 7;
constexpr std::int32_t GlyphGap  = 1;

struct Glyph {
    char        Letter;
    const char *Rows;    // 7 rows of 5: '#' is ink
};

constexpr Glyph Font[] = {
    {'A', ".###.#...##...#######...##...##...#"},
    {'B', "####.#...##...#####.#...##...#####."},
    {'C', ".###.#...##....#....#....#...#.###."},
    {'D', "####.#...##...##...##...##...#####."},
    {'E', "######....#....####.#....#....#####"},
    {'F', "######....#....####.#....#....#...."},
    {'G', ".###.#...##....#.####...##...#.####"},
    {'H', "#...##...##...#######...##...##...#"},
    {'I', ".###...#....#....#....#....#...###."},
    {'J', "..###...#....#....#....#.#..#..##.."},
    {'K', "#...##..#.#.#..##...#.#..#..#.#...#"},
    {'L', "#....#....#....#....#....#....#####"},
    {'M', "#...###.###.#.##.#.##...##...##...#"},
    {'N', "#...###..##.#.##..###...##...##...#"},
    {'O', ".###.#...##...##...##...##...#.###."},
    {'P', "####.#...##...#####.#....#....#...."},
    {'Q', ".###.#...##...##...##.#.##..#..##.#"},
    {'R', "####.#...##...#####.#.#..#..#.#...#"},
    {'S', ".#####....#.....###.....#....#####."},
    {'T', "#####..#....#....#....#....#....#.."},
    {'U', "#...##...##...##...##...##...#.###."},
    {'V', "#...##...##...##...##...#.#.#...#.."},
    {'W', "#...##...##...##.#.##.#.###.###...#"},
    {'X', "#...##...#.#.#...#...#.#.#...##...#"},
    {'Y', "#...##...#.#.#...#....#....#....#.."},
    {'Z', "#####....#...#...#...#...#....#####"},
    {'0', ".###.#...##..###.#.###..##...#.###."},
    {'1', "..#...##....#....#....#....#...###."},
    {'2', ".###.#...#....#...#...#...#...#####"},
    {'3', "####.....#....#.###.....#....#####."},
    {'4', "...#...##..#.#.#..#.#####...#....#."},
    {'5', "######....####.....#....##...#.###."},
    {'6', "..##..#...#....####.#...##...#.###."},
    {'7', "#####....#...#...#...#....#....#..."},
    {'8', ".###.#...##...#.###.#...##...#.###."},
    {'9', ".###.#...##...#.####....#...#..##.."},
    {'!', "..#....#....#....#....#.........#.."},
    {'-', "...............#####..............."},
    {'+', ".......#....#..#####..#....#......."},
    {':', ".......#...................#......."},
    {'.', "..........................##...##.."},
    {'x', "..........#...#.#.#...#...#.#.#...#"},
};

const Glyph *Find(char Letter)
{
    if (Letter >= 'a' && Letter <= 'z' && Letter != 'x') Letter = static_cast<char>(Letter - 'a' + 'A');
    for (const Glyph &One : Font)
        if (One.Letter == Letter) return &One;
    return nullptr;
}

}  // namespace

std::int32_t Canvas::LettersWide(std::string_view Word, std::int32_t Scale)
{
    if (Word.empty()) return 0;
    return static_cast<std::int32_t>(Word.size()) * (GlyphWide + GlyphGap) * Scale - GlyphGap * Scale;
}

bool Canvas::Inked(char Letter, std::int32_t Col, std::int32_t Row)
{
    const Glyph *Shape = Find(Letter);
    return Shape && Col >= 0 && Col < GlyphWide && Row >= 0 && Row < GlyphHigh && Shape->Rows[Row * GlyphWide + Col] == '#';
}

void Canvas::Letters(std::int32_t X, std::int32_t Y, std::string_view Word, std::int32_t Scale, Color Ink)
{
    for (const char Letter : Word) {
        if (const Glyph *Shape = Find(Letter)) {
            for (std::int32_t Row = 0; Row < GlyphHigh; Row++)
                for (std::int32_t Col = 0; Col < GlyphWide; Col++)
                    if (Shape->Rows[Row * GlyphWide + Col] == '#') Fill(X + Col * Scale, Y + Row * Scale, Scale, Scale, Ink);
        }
        X += (GlyphWide + GlyphGap) * Scale;
    }
}

}  // namespace casino
