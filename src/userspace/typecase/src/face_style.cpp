/* face_style.cpp — what a face is within its family: the names it gives
 * (name), how heavy it is and whether it leans (OS/2, head). */

#include "typecase/font.h"

#include <span>

#include "bytes.h"
#include "typecase/text.h"

namespace typecase {

namespace {

enum : std::uint16_t {
    FamilyName        = 1,
    StyleName         = 2,
    TypographicFamily = 16,
    TypographicStyle  = 17,
};

/* OS/2 fsSelection and head macStyle. */
constexpr std::uint16_t SelectItalic = 1u << 0, SelectBold = 1u << 5, SelectOblique = 1u << 9;
constexpr std::uint16_t MacBold = 1u << 0, MacItalic = 1u << 1;

/* Mac Roman's upper half, as Unicode: the encoding of the oldest names. */
constexpr char16_t MacRoman[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5,
    0x00E7, 0x00E9, 0x00E8, 0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, 0x00F2, 0x00F4,
    0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC, 0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6,
    0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8, 0x221E, 0x00B1, 0x2264, 0x2265,
    0x00A5, 0x00B5, 0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8, 0x00BF,
    0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5,
    0x0152, 0x0153, 0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044,
    0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02, 0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1,
    0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4, 0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9,
    0x0131, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7,
};

/* How much a record of the platform, encoding and language is wanted, the
 * more the better: Windows' Unicode in American English, in any language,
 * Unicode's own, then Mac Roman in English; 0 for an encoding not read. */
int Wanted(std::uint16_t Platform, std::uint16_t Encoding, std::uint16_t Language) noexcept
{
    if (Platform == 3 && (Encoding == 1 || Encoding == 10)) return Language == 0x0409 ? 4 : 3;
    if (Platform == 0) return 2;
    if (Platform == 1 && Encoding == 0 && Language == 0) return 1;
    return 0;
}

/* The name's bytes as UTF-8: UTF-16 big-endian, a lone surrogate U+FFFD,
 * or Mac Roman. */
std::string Decode(Bytes Text, bool Wide)
{
    std::u32string Said;
    if (!Wide) {
        for (std::size_t At = 0; At < Text.Size(); At++) {
            const std::uint8_t Byte = Text.U8(At);
            Said += Byte < 0x80 ? char32_t{Byte} : char32_t{MacRoman[Byte - 0x80]};
        }
        return Utf8Of(Said);
    }
    for (std::size_t At = 0; At + 1 < Text.Size(); At += 2) {
        char32_t Unit = Text.U16(At);
        if (Unit >= 0xD800 && Unit < 0xDC00 && At + 3 < Text.Size() && Text.U16(At + 2) >= 0xDC00 &&
            Text.U16(At + 2) < 0xE000) {
            Unit = 0x10000 + ((Unit - 0xD800) << 10) + (Text.U16(At + 2) - 0xDC00);
            At += 2;
        } else if (Unit >= 0xD800 && Unit < 0xE000) {
            Unit = 0xFFFD;
        }
        Said += Unit;
    }
    return Utf8Of(Said);
}

/* The name of Id the name table gives in the encoding most wanted; empty
 * when it gives none that reads. */
std::string NameOf(Bytes Table, std::uint16_t Id)
{
    if (!Table.Holds(0, 6)) return {};
    const std::size_t Records = Table.U16(2), Storage = Table.U16(4);
    int               Best    = 0;
    Bytes             Chosen;
    bool              Wide    = false;
    for (std::size_t Index = 0; Index < Records && Table.Holds(6 + Index * 12, 12); Index++) {
        const std::size_t At = 6 + Index * 12;
        if (Table.U16(At + 6) != Id) continue;
        const std::uint16_t Platform = Table.U16(At), Encoding = Table.U16(At + 2);
        const int           Rank     = Wanted(Platform, Encoding, Table.U16(At + 4));
        const std::size_t   Length = Table.U16(At + 8), Offset = Storage + Table.U16(At + 10);
        if (Rank <= Best || !Table.Holds(Offset, Length)) continue;
        Best   = Rank;
        Chosen = Table.Part(Offset, Length);
        Wide   = Platform != 1;
    }
    return Best ? Decode(Chosen, Wide) : std::string();
}

}  // namespace

void Font::ReadStyle()
{
    const Bytes Whole{std::span(File)};
    const Bytes Names = Whole.Part(Name.Offset, Name.Length);
    Face.Family = NameOf(Names, TypographicFamily);
    if (Face.Family.empty()) Face.Family = NameOf(Names, FamilyName);
    Face.Name = NameOf(Names, TypographicStyle);
    if (Face.Name.empty()) Face.Name = NameOf(Names, StyleName);

    const std::uint16_t Mac   = Whole.Part(Head.Offset, Head.Length).U16(44);
    const Bytes         Os2Of = Whole.Part(Os2.Offset, Os2.Length);
    std::uint16_t       Weight = Os2Of.Holds(0, 6) ? Os2Of.U16(4) : 0;
    if (Weight >= 1 && Weight <= 9) Weight = static_cast<std::uint16_t>(Weight * 100);
    const std::uint16_t Selection = Os2Of.Holds(0, 64) ? Os2Of.U16(62) : 0;
    if (Weight < 1 || Weight > 1000) Weight = (Selection & SelectBold) || (Mac & MacBold) ? 700 : 400;
    Face.Weight = Weight;
    Face.Italic = (Selection & (SelectItalic | SelectOblique)) != 0 || (Mac & MacItalic) != 0;
}

}  // namespace typecase
