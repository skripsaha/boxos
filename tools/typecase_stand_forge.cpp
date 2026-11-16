/* typecase stand — the stand's own fonts, and the damages a real one is put
 * through (typecase_stand_forge.h). */

#include "typecase_stand_forge.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace stand {

namespace {

void AddU16(ByteVector &Into, std::uint32_t Value)
{
    Into.push_back(static_cast<std::uint8_t>(Value >> 8));
    Into.push_back(static_cast<std::uint8_t>(Value));
}

void AddU32(ByteVector &Into, std::uint32_t Value)
{
    AddU16(Into, Value >> 16);
    AddU16(Into, Value & 0xFFFFu);
}

std::uint16_t U16(const ByteVector &From, std::size_t At)
{
    return At + 2 <= From.size() ? static_cast<std::uint16_t>(From[At] << 8 | From[At + 1]) : 0;
}

std::uint32_t U32(const ByteVector &From, std::size_t At)
{
    return static_cast<std::uint32_t>(U16(From, At)) << 16 | U16(From, At + 2);
}

std::uint32_t Tag(const char *Name)
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(Name[0])) << 24 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(Name[1])) << 16 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(Name[2])) << 8 |
           static_cast<unsigned char>(Name[3]);
}

void Pad(ByteVector &Into, std::size_t To)
{
    while (Into.size() % To) Into.push_back(0);
}

/* The sfnt directory and the tables after it, each on four bytes. */
ByteVector Assemble(std::vector<std::pair<const char *, ByteVector>> Tables)
{
    std::sort(Tables.begin(), Tables.end(),
              [](const auto &A, const auto &B) { return std::strcmp(A.first, B.first) < 0; });
    ByteVector Out;
    AddU32(Out, 0x00010000);
    AddU16(Out, static_cast<std::uint32_t>(Tables.size()));
    AddU16(Out, 0), AddU16(Out, 0), AddU16(Out, 0);   /* the search hints, which no reader needs */
    std::uint32_t At = 12 + 16 * static_cast<std::uint32_t>(Tables.size());
    for (const auto &[Name, Data] : Tables) {
        AddU32(Out, Tag(Name));
        AddU32(Out, 0);
        AddU32(Out, At);
        AddU32(Out, static_cast<std::uint32_t>(Data.size()));
        At += static_cast<std::uint32_t>((Data.size() + 3) / 4 * 4);
    }
    for (const auto &Each : Tables) {
        Out.insert(Out.end(), Each.second.begin(), Each.second.end());
        Pad(Out, 4);
    }
    return Out;
}

}  // namespace

void PutU16(ByteVector &Into, std::size_t At, std::uint16_t Value)
{
    if (At + 2 > Into.size()) return;
    Into[At]     = static_cast<std::uint8_t>(Value >> 8);
    Into[At + 1] = static_cast<std::uint8_t>(Value);
}

void PutU32(ByteVector &Into, std::size_t At, std::uint32_t Value)
{
    PutU16(Into, At, static_cast<std::uint16_t>(Value >> 16));
    PutU16(Into, At + 2, static_cast<std::uint16_t>(Value));
}

ByteVector Forge::Build() const
{
    ByteVector Glyf, Loca;
    for (std::size_t Glyph = 0; Glyph <= Glyphs.size(); Glyph++) {
        if (LongLoca) AddU32(Loca, static_cast<std::uint32_t>(Glyf.size()));
        else AddU16(Loca, static_cast<std::uint32_t>(Glyf.size() / 2));
        if (Glyph == Glyphs.size()) break;
        Glyf.insert(Glyf.end(), Glyphs[Glyph].begin(), Glyphs[Glyph].end());
        Pad(Glyf, 2);
    }

    ByteVector Head;
    AddU32(Head, 0x00010000), AddU32(Head, 0x00010000), AddU32(Head, 0), AddU32(Head, 0x5F0F3CF5);
    AddU16(Head, 0), AddU16(Head, UnitsPerEm);
    for (int Index = 0; Index < 4; Index++) AddU32(Head, 0);   /* created, modified */
    for (int Index = 0; Index < 4; Index++) AddU16(Head, 0);   /* the bounding box */
    AddU16(Head, 0), AddU16(Head, 8), AddU16(Head, 2), AddU16(Head, LongLoca ? 1 : 0), AddU16(Head, 0);

    ByteVector Maxp;
    AddU32(Maxp, 0x00005000), AddU16(Maxp, static_cast<std::uint32_t>(Glyphs.size()));

    ByteVector Hhea;
    AddU32(Hhea, 0x00010000), AddU16(Hhea, 800), AddU16(Hhea, static_cast<std::uint16_t>(-200)), AddU16(Hhea, 0);
    for (int Index = 0; Index < 12; Index++) AddU16(Hhea, 0);
    AddU16(Hhea, static_cast<std::uint32_t>(Glyphs.size()));

    ByteVector Hmtx;
    for (std::size_t Glyph = 0; Glyph < Glyphs.size(); Glyph++) {
        AddU16(Hmtx, Glyph < Advances.size() ? Advances[Glyph] : 500);
        AddU16(Hmtx, 0);
    }

    ByteVector Cmap;
    AddU16(Cmap, 0), AddU16(Cmap, static_cast<std::uint32_t>(Maps.size()));
    std::uint32_t At = 4 + 8 * static_cast<std::uint32_t>(Maps.size());
    for (const ForgeMap &Map : Maps) {
        AddU16(Cmap, Map.Platform), AddU16(Cmap, Map.Encoding), AddU32(Cmap, At);
        At += static_cast<std::uint32_t>(Map.Subtable.size());
    }
    for (const ForgeMap &Map : Maps) Cmap.insert(Cmap.end(), Map.Subtable.begin(), Map.Subtable.end());

    return Assemble({{"head", Head}, {"maxp", Maxp}, {"hhea", Hhea}, {"hmtx", Hmtx},
                     {"loca", Loca}, {"glyf", Glyf}, {"cmap", Cmap}});
}

/* Each point's coordinates as deltas, in the shortest form glyf has — none,
 * a byte or a word — and runs of one flag written once with a repeat. */
ByteVector SimpleGlyph(const std::vector<ForgeContour> &Contours)
{
    ByteVector Out;
    AddU16(Out, static_cast<std::uint32_t>(Contours.size()));
    for (int Index = 0; Index < 4; Index++) AddU16(Out, 0);
    std::uint32_t End = 0;
    for (const ForgeContour &Contour : Contours) {
        End += static_cast<std::uint32_t>(Contour.size());
        AddU16(Out, End - 1);
    }
    AddU16(Out, 0);

    std::vector<std::uint8_t> Flags;
    ByteVector                Xs, Ys;
    std::int32_t              LastX = 0, LastY = 0;
    for (const ForgeContour &Contour : Contours) {
        for (const ForgePoint &Point : Contour) {
            std::uint8_t Flag = Point.On ? 1 : 0;
            const std::int32_t Dx = Point.X - LastX, Dy = Point.Y - LastY;
            LastX = Point.X, LastY = Point.Y;
            if (Dx == 0) Flag |= 0x10;
            else if (std::abs(Dx) <= 255) Flag |= static_cast<std::uint8_t>(0x02 | (Dx > 0 ? 0x10 : 0)), Xs.push_back(static_cast<std::uint8_t>(std::abs(Dx)));
            else AddU16(Xs, static_cast<std::uint16_t>(Dx));
            if (Dy == 0) Flag |= 0x20;
            else if (std::abs(Dy) <= 255) Flag |= static_cast<std::uint8_t>(0x04 | (Dy > 0 ? 0x20 : 0)), Ys.push_back(static_cast<std::uint8_t>(std::abs(Dy)));
            else AddU16(Ys, static_cast<std::uint16_t>(Dy));
            Flags.push_back(Flag);
        }
    }
    for (std::size_t Index = 0; Index < Flags.size();) {
        std::size_t Run = 1;
        while (Index + Run < Flags.size() && Flags[Index + Run] == Flags[Index] && Run < 256) Run++;
        if (Run > 1) {
            Out.push_back(static_cast<std::uint8_t>(Flags[Index] | 0x08));
            Out.push_back(static_cast<std::uint8_t>(Run - 1));
        } else {
            Out.push_back(Flags[Index]);
        }
        Index += Run;
    }
    Out.insert(Out.end(), Xs.begin(), Xs.end());
    Out.insert(Out.end(), Ys.begin(), Ys.end());
    return Out;
}

ByteVector CompositeGlyph(const std::vector<ForgeComponent> &Components)
{
    ByteVector Out;
    AddU16(Out, 0xFFFF);
    for (int Index = 0; Index < 4; Index++) AddU16(Out, 0);
    for (std::size_t Index = 0; Index < Components.size(); Index++) {
        const ForgeComponent &Part    = Components[Index];
        const bool            Offsets = Part.Flags & 0x0002;
        const auto            Fits    = [&](std::int32_t V) { return Offsets ? V >= -128 && V <= 127 : V >= 0 && V <= 255; };
        std::uint16_t         Flags   = Part.Flags;
        if (!Fits(Part.First) || !Fits(Part.Second)) Flags |= 0x0001;
        if (Index + 1 < Components.size()) Flags |= 0x0020;
        AddU16(Out, Flags), AddU16(Out, Part.Glyph);
        if (Flags & 0x0001) {
            AddU16(Out, static_cast<std::uint16_t>(Part.First)), AddU16(Out, static_cast<std::uint16_t>(Part.Second));
        } else {
            Out.push_back(static_cast<std::uint8_t>(Part.First)), Out.push_back(static_cast<std::uint8_t>(Part.Second));
        }
        for (std::int16_t Scale : Part.Scale) AddU16(Out, static_cast<std::uint16_t>(Scale));
    }
    return Out;
}

ByteVector MapFormat0(const std::map<char32_t, std::uint16_t> &Of)
{
    ByteVector Out;
    AddU16(Out, 0), AddU16(Out, 262), AddU16(Out, 0);
    for (char32_t Character = 0; Character < 256; Character++) {
        const auto Found = Of.find(Character);
        Out.push_back(Found == Of.end() ? 0 : static_cast<std::uint8_t>(Found->second));
    }
    return Out;
}

/* Runs of characters in a row: a run of glyphs in a row too is a delta, any
 * other a range into the glyph array — both of format 4's ways are kept. */
ByteVector MapFormat4(const std::map<char32_t, std::uint16_t> &Of)
{
    struct Segment {
        std::uint16_t Start, End;
        bool          Delta;
        std::vector<std::uint16_t> Glyphs;
    };
    std::vector<Segment> Segments;
    for (const auto &[Character, Glyph] : Of) {
        if (Character > 0xFFFE) continue;
        if (!Segments.empty() && Segments.back().End + 1u == Character) {
            Segment &Last = Segments.back();
            Last.Delta    = Last.Delta && Last.Glyphs.back() + 1u == Glyph;
            Last.End      = static_cast<std::uint16_t>(Character);
            Last.Glyphs.push_back(Glyph);
        } else {
            Segments.push_back(Segment{static_cast<std::uint16_t>(Character), static_cast<std::uint16_t>(Character), true, {Glyph}});
        }
    }
    Segments.push_back(Segment{0xFFFF, 0xFFFF, true, {0}});

    const std::uint32_t Count = static_cast<std::uint32_t>(Segments.size());
    ByteVector          Out, Array;
    AddU16(Out, 4), AddU16(Out, 0), AddU16(Out, 0);
    AddU16(Out, Count * 2), AddU16(Out, 0), AddU16(Out, 0), AddU16(Out, 0);
    for (const Segment &Each : Segments) AddU16(Out, Each.End);
    AddU16(Out, 0);
    for (const Segment &Each : Segments) AddU16(Out, Each.Start);
    for (const Segment &Each : Segments)
        AddU16(Out, Each.Start == 0xFFFF ? 1 : Each.Delta ? static_cast<std::uint16_t>(Each.Glyphs[0] - Each.Start) : 0);
    for (std::uint32_t Index = 0; Index < Count; Index++) {
        const Segment &Each = Segments[Index];
        if (Each.Delta) {
            AddU16(Out, 0);
            continue;
        }
        AddU16(Out, (Count - Index) * 2 + static_cast<std::uint32_t>(Array.size()));
        for (std::uint16_t Glyph : Each.Glyphs) AddU16(Array, Glyph);
    }
    Out.insert(Out.end(), Array.begin(), Array.end());
    PutU16(Out, 2, static_cast<std::uint16_t>(std::min<std::size_t>(Out.size(), 0xFFFF)));
    return Out;
}

ByteVector MapFormat6(char32_t First, const std::vector<std::uint16_t> &Glyphs)
{
    ByteVector Out;
    AddU16(Out, 6), AddU16(Out, static_cast<std::uint32_t>(10 + Glyphs.size() * 2)), AddU16(Out, 0);
    AddU16(Out, static_cast<std::uint32_t>(First)), AddU16(Out, static_cast<std::uint32_t>(Glyphs.size()));
    for (std::uint16_t Glyph : Glyphs) AddU16(Out, Glyph);
    return Out;
}

ByteVector MapFormat12(const std::map<char32_t, std::uint16_t> &Of, bool ManyToOne)
{
    struct Group {
        std::uint32_t Start, End, Glyph;
    };
    std::vector<Group> Groups;
    for (const auto &[Character, Glyph] : Of) {
        if (!Groups.empty() && Groups.back().End + 1 == Character) {
            Group &Last = Groups.back();
            const std::uint32_t Expected = ManyToOne ? Last.Glyph : Last.Glyph + (Character - Last.Start);
            if (Expected == Glyph) {
                Last.End = Character;
                continue;
            }
        }
        Groups.push_back(Group{Character, Character, Glyph});
    }
    ByteVector Out;
    AddU16(Out, ManyToOne ? 13 : 12), AddU16(Out, 0);
    AddU32(Out, static_cast<std::uint32_t>(16 + Groups.size() * 12)), AddU32(Out, 0);
    AddU32(Out, static_cast<std::uint32_t>(Groups.size()));
    for (const Group &Each : Groups) AddU32(Out, Each.Start), AddU32(Out, Each.End), AddU32(Out, Each.Glyph);
    return Out;
}

/* A collection: each font after the header, its tables' offsets moved by
 * where it lands — a collection's offsets count from the file's start. */
ByteVector Collection(const std::vector<ByteVector> &Fonts)
{
    ByteVector Out;
    AddU32(Out, Tag("ttcf")), AddU32(Out, 0x00010000), AddU32(Out, static_cast<std::uint32_t>(Fonts.size()));
    std::uint32_t At = 12 + 4 * static_cast<std::uint32_t>(Fonts.size());
    for (const ByteVector &Font : Fonts) {
        AddU32(Out, At);
        At += static_cast<std::uint32_t>((Font.size() + 3) / 4 * 4);
    }
    for (const ByteVector &Font : Fonts) {
        const std::uint32_t Base = static_cast<std::uint32_t>(Out.size());
        ByteVector          Moved = Font;
        for (std::uint32_t Record = 0; Record < U16(Font, 4); Record++)
            PutU32(Moved, 12 + Record * 16 + 8, U32(Font, 12 + Record * 16 + 8) + Base);
        Out.insert(Out.end(), Moved.begin(), Moved.end());
        Pad(Out, 4);
    }
    return Out;
}

TablePlace FindTable(const ByteVector &Font, const char *Name)
{
    for (std::uint32_t Record = 12; Record < 12 + 16u * U16(Font, 4); Record += 16)
        if (U32(Font, Record) == Tag(Name)) return TablePlace{Record, U32(Font, Record + 8), U32(Font, Record + 12)};
    return TablePlace{};
}

}  // namespace stand
