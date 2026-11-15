/* typecase stand — Typecase (src/userspace/typecase), the very sources
 * BoxOS links, built for the host under the address and undefined-behaviour
 * sanitizers and held to what it must say (tools/typecase_stand.sh runs it):
 *
 *   typecase_stand outlines FONT [INDEX]          the font in the words of tools/typecase_oracle.py
 *   typecase_stand checks FONT DIR [MUTATIONS]    every check; the forged fonts written into DIR
 *   typecase_stand sheet FONT WIDTH HEIGHT OUT [MAGNIFY]
 *                                                 lines of text in cells of WIDTH by HEIGHT, a PNG,
 *                                                 through the chain a screen of cells asks: the
 *                                                 font, the bitmap font BoxOS carries, the code
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "typecase/chain.h"
#include "typecase/font_source.h"
#include "typecase_stand.h"

namespace stand {

std::vector<std::uint8_t> Load(const char *Path)
{
    std::ifstream In(Path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(In), std::istreambuf_iterator<char>());
}

}  // namespace stand

namespace {

/* A coordinate to 1/64 of a unit, its halves to even as the oracle rounds. */
std::string Num(float Value)
{
    char Text[32];
    std::snprintf(Text, sizeof Text, "%.3f", std::nearbyint(static_cast<double>(Value) * 64) / 64);
    return Text;
}

int Outlines(const char *Path, std::uint32_t Index)
{
    auto Read = typecase::Font::Read(stand::Load(Path), Index);
    if (!Read) {
        std::printf("broken: %s\n", Read.error().Said.c_str());
        return 1;
    }
    const typecase::Font        &Font = *Read;
    const typecase::FontMetrics &M    = Font.Metrics();
    std::printf("font glyphs=%u upem=%u ascender=%d descender=%d gap=%d xheight=%d capheight=%d\n",
                Font.GlyphCount(), M.UnitsPerEm, M.Ascender, M.Descender, M.LineGap, M.XHeight, M.CapHeight);
    for (char32_t Character = 0; Character <= 0x10FFFF; Character++)
        if (typecase::GlyphId Glyph = Font.Find(Character))
            std::printf("U+%04X %u\n", static_cast<unsigned>(Character), Glyph);
    for (std::uint32_t Glyph = 0; Glyph < Font.GlyphCount(); Glyph++) {
        const auto Id = static_cast<typecase::GlyphId>(Glyph);
        std::printf("glyph %u advance=%u\n", Glyph, Font.AdvanceOf(Id));
        auto Shape = Font.OutlineOf(Id);
        if (!Shape) {
            std::printf(" unreadable: %s\n", Shape.error().Said.c_str());
            continue;
        }
        std::size_t At = 0;
        for (typecase::Step Each : Shape->Steps) {
            const int   Count = Each == typecase::Step::Quad ? 2 : 1;
            const char *Name  = Each == typecase::Step::Move ? "M" : Each == typecase::Step::Line ? "L" : "Q";
            std::string Line  = std::string(" ") + Name;
            for (int Index = 0; Index < Count; Index++, At++)
                Line += " " + Num(Shape->Points[At].X) + " " + Num(Shape->Points[At].Y);
            std::printf("%s\n", Line.c_str());
        }
    }
    return 0;
}

int Checks(const char *Path, const char *ForgeDir, int Mutations)
{
    const auto  Font = stand::Load(Path);
    stand::Tally Count;
    stand::CheckRaster(Count);
    stand::CheckForged(Count, ForgeDir);
    stand::CheckGlyphs(Count, Font);
    stand::CheckSources(Count, Font);
    stand::CheckDamage(Count, Font, Mutations);
    std::printf("typecase stand: %d held, %d failed\n", Count.Held, Count.Failed);
    return Count.Failed == 0 ? 0 : 1;
}

/* UTF-8 as characters; a byte that begins none, U+FFFD. */
std::u32string Characters(std::string_view Text)
{
    std::u32string Out;
    for (std::size_t At = 0; At < Text.size();) {
        const auto Lead = static_cast<unsigned char>(Text[At]);
        const int  Need = Lead < 0x80 ? 1 : Lead >= 0xF0 ? 4 : Lead >= 0xE0 ? 3 : Lead >= 0xC0 ? 2 : 0;
        if (Need == 0 || At + static_cast<std::size_t>(Need) > Text.size()) {
            Out += U'�';
            At++;
            continue;
        }
        char32_t Value = Need == 1 ? Lead : Lead & (0x7Fu >> Need);
        for (int Index = 1; Index < Need; Index++) Value = Value << 6 | (Text[At + static_cast<std::size_t>(Index)] & 0x3Fu);
        Out += Value;
        At += static_cast<std::size_t>(Need);
    }
    return Out;
}

std::uint32_t Crc(const std::uint8_t *Data, std::size_t Length, std::uint32_t Crc = 0xFFFFFFFFu)
{
    for (std::size_t Index = 0; Index < Length; Index++) {
        Crc ^= Data[Index];
        for (int Bit = 0; Bit < 8; Bit++) Crc = Crc & 1 ? 0xEDB88320u ^ (Crc >> 1) : Crc >> 1;
    }
    return Crc;
}

void AddU32(std::vector<std::uint8_t> &Into, std::uint32_t Value)
{
    for (int Shift = 24; Shift >= 0; Shift -= 8) Into.push_back(static_cast<std::uint8_t>(Value >> Shift));
}

/* A grey picture as a PNG: rows unfiltered, deflate's stored blocks — a
 * file any viewer opens, made with nothing but this. */
void WritePng(const char *Path, const std::vector<std::uint8_t> &Grey, std::uint32_t Width, std::uint32_t Height)
{
    std::vector<std::uint8_t> Raw;
    for (std::uint32_t Row = 0; Row < Height; Row++) {
        Raw.push_back(0);
        Raw.insert(Raw.end(), Grey.begin() + Row * Width, Grey.begin() + (Row + 1) * Width);
    }
    std::vector<std::uint8_t> Zlib{0x78, 0x01};
    for (std::size_t At = 0; At < Raw.size() || At == 0; At += 65535) {
        const std::size_t Length = std::min<std::size_t>(65535, Raw.size() - At);
        Zlib.push_back(At + Length >= Raw.size() ? 1 : 0);
        Zlib.push_back(static_cast<std::uint8_t>(Length)), Zlib.push_back(static_cast<std::uint8_t>(Length >> 8));
        Zlib.push_back(static_cast<std::uint8_t>(~Length)), Zlib.push_back(static_cast<std::uint8_t>(~Length >> 8));
        Zlib.insert(Zlib.end(), Raw.begin() + static_cast<std::ptrdiff_t>(At),
                    Raw.begin() + static_cast<std::ptrdiff_t>(At + Length));
        if (Raw.empty()) break;
    }
    std::uint32_t A = 1, B = 0;
    for (std::uint8_t Byte : Raw) A = (A + Byte) % 65521, B = (B + A) % 65521;
    AddU32(Zlib, B << 16 | A);

    std::vector<std::uint8_t> File{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    const auto Chunk = [&](const char *Type, const std::vector<std::uint8_t> &Data) {
        AddU32(File, static_cast<std::uint32_t>(Data.size()));
        std::vector<std::uint8_t> Body(Type, Type + 4);
        Body.insert(Body.end(), Data.begin(), Data.end());
        File.insert(File.end(), Body.begin(), Body.end());
        AddU32(File, Crc(Body.data(), Body.size()) ^ 0xFFFFFFFFu);
    };
    std::vector<std::uint8_t> Header;
    AddU32(Header, Width), AddU32(Header, Height);
    Header.insert(Header.end(), {8, 0, 0, 0, 0});
    Chunk("IHDR", Header);
    Chunk("IDAT", Zlib);
    Chunk("IEND", {});
    std::ofstream(Path, std::ios::binary).write(reinterpret_cast<const char *>(File.data()),
                                                static_cast<std::streamsize>(File.size()));
}

int Sheet(const char *Path, std::int32_t Width, std::int32_t Height, const char *Out, std::int32_t Magnify)
{
    auto Read = typecase::Font::Read(stand::Load(Path));
    if (!Read) {
        std::printf("broken: %s\n", Read.error().Said.c_str());
        return 1;
    }
    const typecase::Size Cell = typecase::FitCell(*Read, Width, Height);
    typecase::Chain      Chain;
    Chain.Add(std::make_unique<typecase::FontSource>(std::move(*Read)));
    Chain.Add(stand::BoxFont());

    const char *Lines[] = {
        "The quick brown fox jumps over the lazy dog. 0123456789",
        "Съешь же ещё этих мягких французских булок, да выпей чаю.",
        "Ξεσκεπάζω την ψυχοφθόρα βδελυγμία. ΑΒΓΔΕΖΗΘ αβγδεζηθ",
        "{}[]()<>!?@#$%^&*_+-=/\\|~`'\";:,. il1I| O0o rnm",
        "ÀÉÎÕÜ Ññ Çç ß Łł Ёё Йй Її Ґґ Ўў ŠŽ šž",
        "┌─┬─┐ ╔═╦═╗ ▀▄█▌▐ ░▒▓ ■□▪ ←→↑↓ ≠≤≥ ★ ♠♥♦♣",
        "漢字 😀 \xEE\x80\x80 ⠿ 🬀 ▖▗▘▝",
    };
    std::size_t Columns = 0;
    for (const char *Line : Lines) Columns = std::max(Columns, Characters(Line).size());
    const auto PictureWidth  = static_cast<std::uint32_t>(Columns) * static_cast<std::uint32_t>(Width);
    const auto PictureHeight = static_cast<std::uint32_t>(std::size(Lines)) * static_cast<std::uint32_t>(Height);
    std::vector<std::uint8_t> Grey(PictureWidth * PictureHeight, 0);

    for (std::size_t Row = 0; Row < std::size(Lines); Row++) {
        const std::u32string Text = Characters(Lines[Row]);
        for (std::size_t Column = 0; Column < Text.size(); Column++) {
            const typecase::GlyphImage Image = Chain.Draw(Text[Column], Cell);
            for (std::int32_t Y = 0; Y < Image.Height; Y++)
                for (std::int32_t X = 0; X < Image.Width; X++) {
                    const std::int32_t InX = Image.Left + X, InY = Cell.Baseline + Image.Top + Y;
                    if (InX < 0 || InX >= Width || InY < 0 || InY >= Height) continue;   /* the cell's, no further */
                    Grey[(Row * static_cast<std::size_t>(Height) + static_cast<std::size_t>(InY)) * PictureWidth +
                         Column * static_cast<std::size_t>(Width) + static_cast<std::size_t>(InX)] =
                        Image.Coverage[static_cast<std::size_t>(Y * Image.Width + X)];
                }
        }
    }

    const auto Wide = PictureWidth * static_cast<std::uint32_t>(Magnify);
    const auto Tall = PictureHeight * static_cast<std::uint32_t>(Magnify);
    std::vector<std::uint8_t> Shown(static_cast<std::size_t>(Wide) * Tall);
    for (std::uint32_t Y = 0; Y < Tall; Y++)
        for (std::uint32_t X = 0; X < Wide; X++) {
            const std::uint8_t Ink = Grey[(Y / static_cast<std::uint32_t>(Magnify)) * PictureWidth + X / static_cast<std::uint32_t>(Magnify)];
            Shown[static_cast<std::size_t>(Y) * Wide + X] = static_cast<std::uint8_t>(18 + Ink * 217 / 255);
        }
    WritePng(Out, Shown, Wide, Tall);
    return 0;
}

}  // namespace

int main(int Count, char **Words)
{
    const std::string_view Mode = Count > 1 ? Words[1] : "";
    if (Mode == "outlines" && Count >= 3)
        return Outlines(Words[2], Count > 3 ? static_cast<std::uint32_t>(std::stoul(Words[3])) : 0);
    if (Mode == "checks" && Count >= 4) return Checks(Words[2], Words[3], Count > 4 ? std::stoi(Words[4]) : 1000);
    if (Mode == "sheet" && Count >= 6)
        return Sheet(Words[2], std::stoi(Words[3]), std::stoi(Words[4]), Words[5], Count > 6 ? std::stoi(Words[6]) : 1);
    std::fprintf(stderr, "usage: typecase_stand outlines FONT [INDEX] | checks FONT DIR [MUTATIONS] | "
                         "sheet FONT WIDTH HEIGHT OUT [MAGNIFY]\n");
    return 2;
}
