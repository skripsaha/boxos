/* typecase stand — the named damages a real font is put through: each a
 * way a medium, a careless tool or a hostile hand can leave the bytes, and
 * each must come back from the reader as a Fault, or as a font that reads
 * with what it could not read named — never as a read out of bounds, an
 * overflow or a hang (the stand runs under the sanitizers). */

#include <chrono>
#include <cstdio>
#include <random>
#include <string>

#include "typecase/font.h"
#include "typecase/rasterizer.h"
#include "typecase_stand.h"
#include "typecase_stand_forge.h"

namespace stand {

namespace {

std::uint16_t U16(const ByteVector &From, std::size_t At)
{
    return At + 2 <= From.size() ? static_cast<std::uint16_t>(From[At] << 8 | From[At + 1]) : 0;
}

std::uint32_t U32(const ByteVector &From, std::size_t At)
{
    return static_cast<std::uint32_t>(U16(From, At)) << 16 | U16(From, At + 2);
}

ByteVector Cut(const ByteVector &Font, std::size_t To)
{
    return ByteVector(Font.begin(), Font.begin() + static_cast<std::ptrdiff_t>(std::min(To, Font.size())));
}

/* Where glyph Glyph's record lies in Font, by its loca. */
std::uint32_t RecordOf(const ByteVector &Font, std::uint32_t Glyph)
{
    const TablePlace Head = FindTable(Font, "head"), Loca = FindTable(Font, "loca"), Glyf = FindTable(Font, "glyf");
    const bool       Long = U16(Font, Head.Offset + 50) == 1;
    return Glyf.Offset + (Long ? U32(Font, Loca.Offset + Glyph * 4) : U16(Font, Loca.Offset + Glyph * 2) * 2u);
}

/* The first glyph with an outline whose contours are of Kind: simple
 * (contours > 1) or composite (negative); none when the font has none. */
std::uint32_t FirstGlyph(const ByteVector &Font, bool Composite)
{
    const TablePlace Maxp = FindTable(Font, "maxp");
    for (std::uint32_t Glyph = 0; Glyph < U16(Font, Maxp.Offset + 4); Glyph++) {
        if (RecordOf(Font, Glyph + 1) == RecordOf(Font, Glyph)) continue;
        const auto Contours = static_cast<std::int16_t>(U16(Font, RecordOf(Font, Glyph)));
        if (Composite ? Contours < 0 : Contours > 1) return Glyph;
    }
    return 0xFFFFFFFF;
}

}  // namespace

std::vector<Damage> Damages(const ByteVector &Font)
{
    std::vector<Damage> Out;
    const auto Make = [&](std::string Said, auto Change) {
        ByteVector Bytes = Font;
        Change(Bytes);
        Out.push_back(Damage{std::move(Said), std::move(Bytes)});
    };

    Out.push_back(Damage{"the file cut to nothing", {}});
    Out.push_back(Damage{"the file cut inside its header", Cut(Font, 11)});
    Out.push_back(Damage{"the file cut inside its directory", Cut(Font, 20)});
    Make("the directory claims 65535 tables", [](ByteVector &B) { PutU16(B, 4, 0xFFFF); });
    Make("the version is CFF's", [](ByteVector &B) { PutU32(B, 0, 0x4F54544F); });
    Make("the version is no font's", [](ByteVector &B) { PutU32(B, 0, 0xDEADBEEF); });

    for (const char *Name : {"head", "maxp", "hhea", "hmtx", "loca", "glyf", "cmap"}) {
        const TablePlace Table = FindTable(Font, Name);
        const std::string Of   = std::string(Name) + ": ";
        Out.push_back(Damage{Of + "the file cut at its start", Cut(Font, Table.Offset)});
        Out.push_back(Damage{Of + "the file cut in its middle", Cut(Font, Table.Offset + Table.Length / 2)});
        Out.push_back(Damage{Of + "the file cut a byte short of its end", Cut(Font, Table.Offset + Table.Length - 1)});
        Make(Of + "its offset past the end", [&](ByteVector &B) { PutU32(B, Table.Record + 8, static_cast<std::uint32_t>(B.size())); });
        Make(Of + "its length past the end", [&](ByteVector &B) { PutU32(B, Table.Record + 12, 0xFFFFFFFF); });
        Make(Of + "its length 0", [&](ByteVector &B) { PutU32(B, Table.Record + 12, 0); });
        Make(Of + "missing", [&](ByteVector &B) { PutU32(B, Table.Record, 0x78787878); });
    }

    const std::uint32_t Head = FindTable(Font, "head").Offset;
    Make("head: its magic number wrong", [&](ByteVector &B) { PutU32(B, Head + 12, 0x12345678); });
    Make("head: 0 units to the em", [&](ByteVector &B) { PutU16(B, Head + 18, 0); });
    Make("head: 16385 units to the em", [&](ByteVector &B) { PutU16(B, Head + 18, 16385); });
    Make("head: loca of format 2", [&](ByteVector &B) { PutU16(B, Head + 50, 2); });
    Make("head: loca of the other format", [&](ByteVector &B) { PutU16(B, Head + 50, U16(B, Head + 50) ^ 1u); });

    const std::uint32_t Maxp = FindTable(Font, "maxp").Offset, Hhea = FindTable(Font, "hhea").Offset;
    const std::uint16_t Glyphs = U16(Font, Maxp + 4);
    Make("maxp: no glyphs", [&](ByteVector &B) { PutU16(B, Maxp + 4, 0); });
    Make("maxp: 65535 glyphs", [&](ByteVector &B) { PutU16(B, Maxp + 4, 0xFFFF); });
    Make("hhea: no advances", [&](ByteVector &B) { PutU16(B, Hhea + 34, 0); });
    Make("hhea: more advances than glyphs", [&](ByteVector &B) { PutU16(B, Hhea + 34, static_cast<std::uint16_t>(Glyphs + 1)); });

    const TablePlace Loca = FindTable(Font, "loca");
    const bool       Long = U16(Font, Head + 50) == 1;
    const std::uint32_t Simple = FirstGlyph(Font, false), Composite = FirstGlyph(Font, true);
    const auto PutPlace = [&](ByteVector &B, std::uint32_t Glyph, std::uint32_t Value) {
        if (Long) PutU32(B, Loca.Offset + Glyph * 4, Value);
        else PutU16(B, Loca.Offset + Glyph * 2, static_cast<std::uint16_t>(Value / 2));
    };
    if (Simple != 0xFFFFFFFF) {
        const std::uint32_t Record   = RecordOf(Font, Simple);
        const std::uint32_t Contours = U16(Font, Record);
        const std::string   Of       = "glyph " + std::to_string(Simple) + ": ";
        Make(Of + "its place runs backwards", [&](ByteVector &B) { PutPlace(B, Simple + 1, 0); });
        Make(Of + "its place past glyf", [&](ByteVector &B) { PutPlace(B, Simple + 1, Long ? 0xFFFFFFF0u : 0x1FFF0u); });
        Make(Of + "32767 contours", [&](ByteVector &B) { PutU16(B, Record, 0x7FFF); });
        Make(Of + "contour ends running backwards", [&](ByteVector &B) { PutU16(B, Record + 12, 0); });
        Make(Of + "its last point numbered 65535", [&](ByteVector &B) { PutU16(B, Record + 10 + 2 * (Contours - 1), 0xFFFF); });
        Make(Of + "instructions longer than its record", [&](ByteVector &B) { PutU16(B, Record + 10 + 2 * Contours, 0xFFFF); });
        Make(Of + "a flag repeated past its points", [&](ByteVector &B) {
            const std::uint32_t Flags = Record + 12 + 2 * Contours + U16(B, Record + 10 + 2 * Contours);
            if (Flags + 1 < B.size()) B[Flags] = 0x09, B[Flags + 1] = 0xFF;
        });
        Make(Of + "its record cut before its coordinates", [&](ByteVector &B) {
            PutPlace(B, Simple + 1, Record - FindTable(B, "glyf").Offset + 12 + 2 * Contours);
        });
    }
    if (Composite != 0xFFFFFFFF) {
        const std::uint32_t Record = RecordOf(Font, Composite);
        const std::string   Of     = "glyph " + std::to_string(Composite) + " (composite): ";
        Make(Of + "a component that is itself", [&](ByteVector &B) { PutU16(B, Record + 12, static_cast<std::uint16_t>(Composite)); });
        Make(Of + "a component past the font's glyphs", [&](ByteVector &B) { PutU16(B, Record + 12, 0xFFFF); });
        Make(Of + "components that never end", [&](ByteVector &B) { PutU16(B, Record + 10, U16(B, Record + 10) | 0x0020); });
        Make(Of + "pinned by points instead of moved", [&](ByteVector &B) { PutU16(B, Record + 10, U16(B, Record + 10) & ~0x0002u); });
    }

    const std::uint32_t Cmap = FindTable(Font, "cmap").Offset;
    Make("cmap: 65535 encodings", [&](ByteVector &B) { PutU16(B, Cmap + 2, 0xFFFF); });
    for (std::uint32_t Record = 0; Record < U16(Font, Cmap + 2); Record++) {
        const std::uint32_t Sub    = Cmap + U32(Font, Cmap + 4 + Record * 8 + 4);
        const std::uint16_t Format = U16(Font, Sub);
        const std::string   Of     = "cmap " + std::to_string(Record) + " (format " + std::to_string(Format) + "): ";
        Make(Of + "its subtable past the table", [&](ByteVector &B) { PutU32(B, Cmap + 4 + Record * 8 + 4, 0xFFFFFFF0); });
        if (Format == 4) {
            const std::uint32_t Seg2 = U16(Font, Sub + 6);
            Make(Of + "an odd count of segments", [&](ByteVector &B) { PutU16(B, Sub + 6, static_cast<std::uint16_t>(Seg2 | 1)); });
            Make(Of + "no segments", [&](ByteVector &B) { PutU16(B, Sub + 6, 0); });
            Make(Of + "65534 segments", [&](ByteVector &B) { PutU16(B, Sub + 6, 0xFFFE); });
            Make(Of + "its ends out of order", [&](ByteVector &B) { PutU16(B, Sub + 14 + 2, 0); });
            Make(Of + "a range offset past the table", [&](ByteVector &B) {
                for (std::uint32_t Segment = 0; Segment < Seg2 / 2; Segment++)
                    PutU16(B, Sub + 16 + 3 * Seg2 + 2 * Segment, 0xFFFE);
            });
        }
        if (Format == 12) {
            Make(Of + "4294967295 groups", [&](ByteVector &B) { PutU32(B, Sub + 12, 0xFFFFFFFF); });
            Make(Of + "its groups out of order", [&](ByteVector &B) { PutU32(B, Sub + 16 + 12, 0); });
            Make(Of + "a group past U+10FFFF", [&](ByteVector &B) { PutU32(B, Sub + 16 + 4, 0x110000); });
        }
    }
    return Out;
}

namespace {

/* Everything a screen would ask of a font that read: every glyph's outline
 * drawn at Em, and the map asked for the characters of the planes in use. */
void Exercise(const typecase::Font &Font, float Em, std::uint32_t Every)
{
    typecase::Rasterizer Pen;
    const float          Scale  = Em / Font.Metrics().UnitsPerEm;
    const auto           Margin = static_cast<std::int32_t>(Em) + 1;
    for (std::uint32_t Glyph = 0; Glyph < Font.GlyphCount(); Glyph += Every) {
        const auto Id = static_cast<typecase::GlyphId>(Glyph);
        if (!Font.Readable(Id)) continue;
        if (auto Shape = Font.OutlineOf(Id))
            (void)Pen.Draw(*Shape, Scale, Scale, typecase::Bounds{-Margin, -2 * Margin, 2 * Margin, Margin},
                           Font.AdvanceOf(Id) * Scale);
    }
    for (char32_t Character = 0; Character < 0x30000; Character += Every) (void)Font.Find(Character);
}

}  // namespace

void CheckDamage(Tally &Count, const ByteVector &Font, int Mutations)
{
    int Faulted = 0, Read = 0, Named = 0;
    bool Said = true;
    for (const Damage &Each : Damages(Font)) {
        const auto Made = typecase::Font::Read(Each.Bytes);
        if (!Made) {
            Faulted++;
            Said = Said && !Made.error().Said.empty();
            std::printf("  %s: %s\n", Each.Said.c_str(), Made.error().Said.c_str());
            continue;
        }
        Read++;
        if (Made->Damage()) Named++;
        std::printf("  %s: reads%s%s\n", Each.Said.c_str(), Made->Damage() ? "; " : "",
                    Made->Damage() ? Made->Damage()->Said.c_str() : "");
        Exercise(*Made, 16, 1);
    }
    Count.Check(Said && Faulted > 0, "named damages: " + std::to_string(Faulted) + " said as faults, " +
                                         std::to_string(Read) + " read (" + std::to_string(Named) +
                                         " with the glyphs that do not read named), every one drawn whole");

    std::mt19937                       Random(1115);
    std::uniform_int_distribution<int> Kind(0, 3), Times(1, 8);
    std::uniform_real_distribution<float> Em(4, 96);
    int Took = 0, Refused = 0;
    const auto Start = std::chrono::steady_clock::now();
    for (int Each = 0; Each < Mutations; Each++) {
        ByteVector Bytes = Font;
        for (int Change = Times(Random); Change > 0 && !Bytes.empty(); Change--) {
            const std::size_t At = std::uniform_int_distribution<std::size_t>(0, Bytes.size() - 1)(Random);
            switch (Kind(Random)) {
            case 0: Bytes[At] = static_cast<std::uint8_t>(Bytes[At] ^ (1u << (Random() % 8))); break;
            case 1: Bytes[At] = 0xFF; break;
            case 2: Bytes[At] = 0; break;
            default: Bytes.resize(At); break;
            }
        }
        const auto Made = typecase::Font::Read(std::move(Bytes));
        if (!Made) {
            Refused++;
            continue;
        }
        Took++;
        Exercise(*Made, Em(Random), 7);
    }
    const auto Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
    char Line[200];
    std::snprintf(Line, sizeof Line, "%d mutations of the font: %d refused, %d read and drawn, in %.1f s",
                  Mutations, Refused, Took, Seconds);
    Count.Check(Refused + Took == Mutations, Line);
}

}  // namespace stand
