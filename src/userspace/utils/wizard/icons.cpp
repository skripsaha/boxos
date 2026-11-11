/* icons.cpp — the sixteen pictures, in the order of Ingredients(). */

#include "icons.h"

#include <array>
#include <string_view>

#include "lore.h"

namespace wizard {

namespace {

using Picture = std::array<std::string_view, IconSize>;

constexpr std::array<Picture, IngredientCount> Pictures = {{
    // mandrake root: leaves, a face, arms and legs
    {".n.NN.n.", "..nnNn..", "..tttt..", ".tktktt.", ".tttttt.", "t.tttt.t", "..tbbt..", ".tb..bt."},
    // toadstool
    {"..rrrr..", ".rwrrwr.", "rrrwrrrr", "rwrrrrwr", ".RRRRRR.", "...ss...", "...sS...", "..ssSS.."},
    // raven feather
    {"......kk", ".....kGk", "....kGgk", "...kGGgk", "..kGGgk.", ".kGgk...", "kbk.....", "b......."},
    // moonstone
    {"..kkkk..", ".kwwcck.", "kwwhccuk", "kwcccuuk", "kcccuuuk", ".kcuuuk.", "..kkkk..", "........"},
    // dragon scale
    {"...kk...", "..krrk..", ".krorrk.", "kroyorRk", "krorrRRk", ".krrRRk.", "..kRRk..", "...kk..."},
    // frog eye
    {"..kkkk..", ".knlllnk", "knlyyynk", "nlykkyln", "nlykkyln", "knlyyynk", ".knnnnk.", "..kkkk.."},
    // stardust in a vial
    {"...tt...", "...kk...", "..khhk..", ".kppypk.", ".kpypPk.", ".kPpPyk.", ".kPPPPk.", "..kkkk.."},
    // salamander tail
    {".....ooo", "....oyoo", "...oooo.", "..ooyoo.", ".oooo...", "oyoo....", "ooo.....", "oo......"},
    // moon fern
    {"...nn...", "..nNnn..", ".nNnNnn.", "..nNn...", ".nNnNnn.", "...Nn...", "...Nn...", "...bb..."},
    // old bone
    {".wk..kw.", "wwwkkwww", ".kwwwwk.", "..kwwk..", "..kwwk..", ".kwwwwk.", "wwwkkwww", ".wk..kw."},
    // frost berries
    {".....nn.", "...cnNn.", "..cCCCc.", ".cchcCCc", ".cCCCCCc", "..cCCCc.", "...ccc..", "........"},
    // mirror shard
    {"...k....", "..kwk...", ".kwcuk..", ".kwcck..", "kwcccuk.", "kwwcuuk.", ".kkkuk..", "....k..."},
    // glow moss
    {"..l..l..", ".lnlnnl.", "lnnlnnnl", "nnNnnNnn", "nNnnNnnN", ".nNNNNn.", "..kbbk..", "...kk..."},
    // beetle shell
    {"..kkkk..", ".kcCCck.", "kcCCCCck", "kCCCkCCk", "kCCCkCCk", "kCCCkCCk", ".kCCCCk.", "..kkkk.."},
    // cobweb
    {"w..w..w.", ".w.w.w..", "..www...", "wwwkwwww", "..www...", ".w.w.w..", "w..w..w.", "...w...."},
    // old parchment
    {".kkkkkk.", "ktssssk.", ".ksbbsk.", ".kssssk.", ".ksbbbk.", ".kssssk.", "ktssssk.", ".kkkkkk."},
}};

constexpr Color Ink(char Mark) noexcept
{
    switch (Mark) {
    case 'k': return Rgb(0x1A, 0x12, 0x18);
    case 'w': return Rgb(0xF0, 0xEC, 0xE0);
    case 'h': return Rgb(0xFF, 0xFF, 0xFF);
    case 'g': return Rgb(0x9A, 0xA0, 0xB0);
    case 'G': return Rgb(0x4A, 0x4E, 0x5E);
    case 'b': return Rgb(0x8A, 0x5A, 0x2E);
    case 't': return Rgb(0xC8, 0x9A, 0x62);
    case 'r': return Rgb(0xD0, 0x38, 0x30);
    case 'R': return Rgb(0x80, 0x1E, 0x24);
    case 'o': return Rgb(0xF0, 0x84, 0x28);
    case 'y': return Rgb(0xFF, 0xE0, 0x60);
    case 'l': return Rgb(0xA8, 0xF0, 0x70);
    case 'n': return Rgb(0x48, 0xA8, 0x48);
    case 'N': return Rgb(0x24, 0x68, 0x3A);
    case 'c': return Rgb(0x58, 0xC8, 0xE8);
    case 'C': return Rgb(0x28, 0x7E, 0x98);
    case 'u': return Rgb(0x88, 0x90, 0xE8);
    case 'p': return Rgb(0xB0, 0x78, 0xF0);
    case 'P': return Rgb(0x6A, 0x3E, 0xA8);
    case 's': return Rgb(0xE8, 0xD0, 0x98);
    case 'S': return Rgb(0xB8, 0x98, 0x68);
    default:  return Rgb(0xFF, 0x00, 0xFF);
    }
}

}  // namespace

void DrawIcon(Canvas &Out, int X, int Y, std::size_t Which, int Lit, int Glint)
{
    const Picture &Shape = Pictures[Which];
    for (int Row = 0; Row < IconSize; Row++) {
        for (int Column = 0; Column < IconSize; Column++) {
            const char Mark = Shape[static_cast<std::size_t>(Row)][static_cast<std::size_t>(Column)];
            if (Mark == '.') continue;
            // The top and the left catch the light, the bottom and the right keep the dark.
            const int Slope = 6 * (IconSize - 1 - Row - Column) - 40;
            const int Level = std::clamp(Lit + Slope, 40, 400);
            Color Here = Scaled(Ink(Mark), Level, Level, Level);
            if (Glint > 0 && Mark != 'k') Here = Lighter(Here, Glint / 4 + (Row + Column) % 2 * Glint / 12);
            Out.Put(X + Column, Y + Row, Here);
        }
    }
}

void DrawIconOnFrame(painter::tui::Frame &Out, int X, int Y, std::size_t Which)
{
    const Picture &Shape = Pictures[Which];
    for (int Row = 0; Row < IconSize; Row++) {
        for (int Column = 0; Column < IconSize; Column++) {
            const char Mark = Shape[static_cast<std::size_t>(Row)][static_cast<std::size_t>(Column)];
            if (Mark != '.') Out.PutPixel(painter::core::Point{X + Column, Y + Row}, Ink(Mark));
        }
    }
}

Color IconColor(std::size_t Which)
{
    return Color::FromRaw(Ingredients()[Which].Color);
}

}  // namespace wizard
