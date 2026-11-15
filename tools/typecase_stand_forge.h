#ifndef TYPECASE_STAND_FORGE_H
#define TYPECASE_STAND_FORGE_H

/* The stand's own fonts: small TrueType files made here, byte by byte, to
 * hold each path of the reader a real font may never take — every character
 * map format, components scaled, turned, pinned by a point, nested to the
 * limit and past it, a component that holds itself, a collection — and the
 * named damages a real font is put through. */

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace stand {

using ByteVector = std::vector<std::uint8_t>;

struct ForgePoint {
    std::int32_t X;
    std::int32_t Y;
    bool         On;
};
using ForgeContour = std::vector<ForgePoint>;

struct ForgeComponent {
    std::uint16_t Glyph;
    std::uint16_t Flags;          /* as glyf writes them, MORE_COMPONENTS set by the forge */
    std::int32_t  First, Second;  /* offsets, or the points pinned */
    std::vector<std::int16_t> Scale;   /* none, one, two or four 2.14 numbers, as Flags say */
};

struct ForgeMap {
    std::uint16_t Platform;
    std::uint16_t Encoding;
    ByteVector    Subtable;
};

struct Forge {
    std::uint16_t           UnitsPerEm = 1000;
    bool                    LongLoca   = false;
    std::vector<ByteVector> Glyphs;      /* records; empty for a glyph of no outline */
    std::vector<std::uint16_t> Advances;
    std::vector<ForgeMap>   Maps;

    ByteVector Build() const;
};

ByteVector SimpleGlyph(const std::vector<ForgeContour> &Contours);
ByteVector CompositeGlyph(const std::vector<ForgeComponent> &Components);

ByteVector MapFormat0(const std::map<char32_t, std::uint16_t> &Of);
ByteVector MapFormat4(const std::map<char32_t, std::uint16_t> &Of);
ByteVector MapFormat6(char32_t First, const std::vector<std::uint16_t> &Glyphs);
ByteVector MapFormat12(const std::map<char32_t, std::uint16_t> &Of, bool ManyToOne = false);

ByteVector Collection(const std::vector<ByteVector> &Fonts);

/* Where a table lies in a font's bytes; Length 0 when it has none. */
struct TablePlace {
    std::uint32_t Record = 0;   /* its record in the directory */
    std::uint32_t Offset = 0;
    std::uint32_t Length = 0;
};
TablePlace FindTable(const ByteVector &Font, const char *Tag);

void PutU16(ByteVector &Into, std::size_t At, std::uint16_t Value);
void PutU32(ByteVector &Into, std::size_t At, std::uint32_t Value);

/* A damage: what it does to a font, said, and the bytes it leaves. */
struct Damage {
    std::string Said;
    ByteVector  Bytes;
};
std::vector<Damage> Damages(const ByteVector &Font);

}  // namespace stand

#endif  // TYPECASE_STAND_FORGE_H
