/* glyph_reader.cpp — a glyph's outline out of glyf. */

#include "glyph_reader.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace typecase {

namespace {

/* A glyph's points are numbered in sixteen bits, by hinting and by the
 * composites that pin one glyph's point to another's: no glyph has more,
 * and a composite that would gather more — a component used over and over —
 * is not a glyph but a way to exhaust memory. */
constexpr std::size_t MostPoints = 65535;

/* Components nest, sixteen composites deep at most above a simple glyph; a
 * font's own say (maxp) is not trusted, and a component that holds itself,
 * at any depth, is a cycle. */
constexpr std::size_t DeepestNesting = 16;

/* TrueType's points as a glyph record holds them: on the curve or a control
 * between two, and where each contour ends. */
struct Points {
    std::vector<Point>         At;
    std::vector<std::uint8_t>  On;
    std::vector<std::uint16_t> Ends;   /* each contour's last point */
};

enum : std::uint8_t {
    OnCurve      = 0x01,
    XShort       = 0x02,
    YShort       = 0x04,
    Repeat       = 0x08,
    XSameOrPlus  = 0x10,
    YSameOrPlus  = 0x20,
};

enum : std::uint16_t {
    ArgsAreWords       = 0x0001,
    ArgsAreOffsets     = 0x0002,
    HasScale           = 0x0008,
    MoreComponents     = 0x0020,
    HasXYScale         = 0x0040,
    HasTwoByTwo        = 0x0080,
    ScaledOffset       = 0x0800,
    UnscaledOffset     = 0x1000,
};

/* What does not read, and in which glyph — the one asked for, or one of
 * its components (ReadOutline says which). */
struct Broken {
    GlyphId     Glyph;
    std::string What;
};

Broken Says(GlyphId Glyph, std::string_view What) { return Broken{Glyph, std::string(What)}; }

/* The glyph's record in glyf, as loca places it; empty for a glyph with no
 * outline, the space. */
std::expected<Bytes, Broken> Record(const GlyphTables &Tables, GlyphId Glyph)
{
    if (Glyph >= Tables.Glyphs)
        return std::unexpected(Says(Glyph, std::format("is past the font's {} glyphs", Tables.Glyphs)));
    const std::size_t Start = Tables.LongOffsets ? Tables.Loca.U32(Glyph * 4u) : Tables.Loca.U16(Glyph * 2u) * 2u;
    const std::size_t End =
        Tables.LongOffsets ? Tables.Loca.U32((Glyph + 1u) * 4u) : Tables.Loca.U16((Glyph + 1u) * 2u) * 2u;
    if (End < Start) return std::unexpected(Says(Glyph, "ends before it begins (loca)"));
    if (!Tables.Glyf.Holds(Start, End - Start)) return std::unexpected(Says(Glyph, "lies past the table (loca)"));
    return Tables.Glyf.Part(Start, End - Start);
}

/* One axis of a simple glyph's coordinates, from At: each a delta from the
 * last, a byte (its sign in the flags) or a word, or none at all. */
std::optional<std::size_t> ReadAxis(Bytes Record, std::size_t At, const std::vector<std::uint8_t> &Flags,
                                    std::uint8_t Short, std::uint8_t SameOrPlus, std::vector<Point> &Into,
                                    std::size_t First, bool IsX)
{
    std::int32_t Value = 0;
    for (std::size_t Index = 0; Index < Flags.size(); Index++) {
        const std::uint8_t Flag = Flags[Index];
        if (Flag & Short) {
            if (!Record.Holds(At, 1)) return std::nullopt;
            const std::int32_t Delta = Record.U8(At++);
            Value += Flag & SameOrPlus ? Delta : -Delta;
        } else if (!(Flag & SameOrPlus)) {
            if (!Record.Holds(At, 2)) return std::nullopt;
            Value += Record.I16(At);
            At += 2;
        }
        (IsX ? Into[First + Index].X : Into[First + Index].Y) = static_cast<float>(Value);
    }
    return At;
}

std::optional<Broken> ReadSimple(Bytes Record, GlyphId Glyph, std::size_t Contours, Points &Into)
{
    if (!Record.Holds(10, Contours * 2 + 2)) return Says(Glyph, "has contour ends past its record");

    const std::size_t Base = Into.At.size();
    std::size_t       Count = 0;
    for (std::size_t Index = 0; Index < Contours; Index++) {
        const std::size_t End = Record.U16(10 + Index * 2);
        if (Index > 0 && End < Count) return Says(Glyph, "has a contour that ends before the last");
        Count = End + 1;
    }
    if (Base + Count > MostPoints) return Says(Glyph, "has more points than a glyph can number");

    std::size_t At = 10 + Contours * 2;
    At += 2 + Record.U16(At);   /* the glyph's hinting instructions, passed over */

    std::vector<std::uint8_t> Flags;
    Flags.reserve(Count);
    while (Flags.size() < Count) {
        if (!Record.Holds(At, 1)) return Says(Glyph, "has flags past its record");
        const std::uint8_t Flag = Record.U8(At++);
        std::size_t        Times = 1;
        if (Flag & Repeat) {
            if (!Record.Holds(At, 1)) return Says(Glyph, "has flags past its record");
            Times += Record.U8(At++);
        }
        if (Times > Count - Flags.size()) return Says(Glyph, "repeats a flag past its last point");
        Flags.insert(Flags.end(), Times, Flag);
    }

    Into.At.resize(Base + Count);
    const auto AfterX = ReadAxis(Record, At, Flags, XShort, XSameOrPlus, Into.At, Base, true);
    if (!AfterX || !ReadAxis(Record, *AfterX, Flags, YShort, YSameOrPlus, Into.At, Base, false))
        return Says(Glyph, "has coordinates past its record");

    for (std::uint8_t Flag : Flags) Into.On.push_back(static_cast<std::uint8_t>(Flag & OnCurve));
    for (std::size_t Index = 0; Index < Contours; Index++)
        Into.Ends.push_back(static_cast<std::uint16_t>(Base + Record.U16(10 + Index * 2)));
    return std::nullopt;
}

std::optional<Broken> ReadInto(const GlyphTables &Tables, GlyphId Glyph, Points &Into, std::vector<GlyphId> &Within);

/* A composite's components, each read whole on its own, transformed, moved
 * into place — by an offset, or by pinning one of its points to one of the
 * points gathered so far — and added. */
std::optional<Broken> ReadComposite(const GlyphTables &Tables, Bytes Record, GlyphId Glyph, Points &Into,
                                   std::vector<GlyphId> &Within)
{
    std::size_t   At = 10;
    std::uint16_t Flags;
    do {
        if (!Record.Holds(At, 4)) return Says(Glyph, "has a component past its record");
        Flags = Record.U16(At);
        const GlyphId Part = Record.U16(At + 2);
        At += 4;

        const std::size_t ArgsLength = Flags & ArgsAreWords ? 4 : 2;
        const std::size_t ScaleLength =
            Flags & HasTwoByTwo ? 8 : Flags & HasXYScale ? 4 : Flags & HasScale ? 2 : 0;
        if (!Record.Holds(At, ArgsLength + ScaleLength)) return Says(Glyph, "has a component past its record");

        std::int32_t First, Second;
        if (Flags & ArgsAreWords) {
            First  = Flags & ArgsAreOffsets ? Record.I16(At) : Record.U16(At);
            Second = Flags & ArgsAreOffsets ? Record.I16(At + 2) : Record.U16(At + 2);
        } else {
            First  = Flags & ArgsAreOffsets ? static_cast<std::int8_t>(Record.U8(At)) : Record.U8(At);
            Second = Flags & ArgsAreOffsets ? static_cast<std::int8_t>(Record.U8(At + 1)) : Record.U8(At + 1);
        }
        At += ArgsLength;

        /* x' = A x + C y, y' = B x + D y. */
        float A = 1, B = 0, C = 0, D = 1;
        if (Flags & HasTwoByTwo) {
            A = Record.F2Dot14(At), B = Record.F2Dot14(At + 2), C = Record.F2Dot14(At + 4), D = Record.F2Dot14(At + 6);
        } else if (Flags & HasXYScale) {
            A = Record.F2Dot14(At), D = Record.F2Dot14(At + 2);
        } else if (Flags & HasScale) {
            A = D = Record.F2Dot14(At);
        }
        At += ScaleLength;

        Points Component;
        if (auto Failed = ReadInto(Tables, Part, Component, Within)) return Failed;
        for (Point &Each : Component.At)
            Each = Point{A * Each.X + C * Each.Y, B * Each.X + D * Each.Y};

        float Dx, Dy;
        if (Flags & ArgsAreOffsets) {
            Dx = static_cast<float>(First), Dy = static_cast<float>(Second);
            /* Apple's fonts may ask for the offset to be scaled with the
             * component; it is the exception, said by its flag. */
            if ((Flags & ScaledOffset) && !(Flags & UnscaledOffset) && ScaleLength > 0) {
                Dx *= std::hypot(A, C);
                Dy *= std::hypot(D, B);
            }
        } else {
            const auto Pinned = static_cast<std::size_t>(First), Own = static_cast<std::size_t>(Second);
            if (Pinned >= Into.At.size() || Own >= Component.At.size())
                return Says(Glyph, "pins a component by a point it does not have");
            Dx = Into.At[Pinned].X - Component.At[Own].X;
            Dy = Into.At[Pinned].Y - Component.At[Own].Y;
        }

        const std::size_t Base = Into.At.size();
        if (Base + Component.At.size() > MostPoints)
            return Says(Glyph, "gathers more points than a glyph can number");
        for (const Point &Each : Component.At) Into.At.push_back(Point{Each.X + Dx, Each.Y + Dy});
        Into.On.insert(Into.On.end(), Component.On.begin(), Component.On.end());
        for (std::uint16_t End : Component.Ends) Into.Ends.push_back(static_cast<std::uint16_t>(Base + End));
    } while (Flags & MoreComponents);
    return std::nullopt;
}

std::optional<Broken> ReadInto(const GlyphTables &Tables, GlyphId Glyph, Points &Into, std::vector<GlyphId> &Within)
{
    if (std::find(Within.begin(), Within.end(), Glyph) != Within.end())
        return Says(Glyph, "contains itself");
    if (Within.size() > DeepestNesting)
        return Says(Within.front(), std::format("nests components deeper than {}", DeepestNesting));

    const auto Found = Record(Tables, Glyph);
    if (!Found) return Found.error();
    if (Found->Size() == 0) return std::nullopt;
    if (!Found->Holds(0, 10)) return Says(Glyph, "is shorter than a glyph's header");

    const std::int16_t Contours = Found->I16(0);
    if (Contours >= 0) return ReadSimple(*Found, Glyph, static_cast<std::size_t>(Contours), Into);

    Within.push_back(Glyph);
    auto Failed = ReadComposite(Tables, *Found, Glyph, Into, Within);
    Within.pop_back();
    return Failed;
}

Point Between(Point A, Point B) noexcept { return Point{(A.X + B.X) / 2, (A.Y + B.Y) / 2}; }

/* TrueType's points as steps: two controls in a row have a point on the
 * curve between them, unwritten, and a contour may begin off the curve. */
Outline Trace(const Points &From)
{
    Outline     Shape;
    std::size_t Start = 0;
    for (std::uint16_t Last : From.Ends) {
        const std::size_t First = Start, End = Last;
        Start = End + 1;
        if (End <= First) continue;   /* a single point: an anchor, no ink */

        Point       Begin;
        std::size_t Next = First, Until = End;
        if (From.On[First]) {
            Begin = From.At[First];
            Next  = First + 1;
        } else if (From.On[End]) {
            Begin = From.At[End];
            Until = End - 1;
        } else {
            Begin = Between(From.At[First], From.At[End]);
        }

        Shape.Steps.push_back(Step::Move);
        Shape.Points.push_back(Begin);
        std::optional<Point> Control;
        for (std::size_t Index = Next; Index <= Until; Index++) {
            const Point Here = From.At[Index];
            if (From.On[Index]) {
                if (Control) {
                    Shape.Steps.push_back(Step::Quad);
                    Shape.Points.push_back(*Control);
                    Control.reset();
                } else {
                    Shape.Steps.push_back(Step::Line);
                }
                Shape.Points.push_back(Here);
            } else {
                if (Control) {
                    Shape.Steps.push_back(Step::Quad);
                    Shape.Points.push_back(*Control);
                    Shape.Points.push_back(Between(*Control, Here));
                }
                Control = Here;
            }
        }
        if (Control) {
            Shape.Steps.push_back(Step::Quad);
            Shape.Points.push_back(*Control);
            Shape.Points.push_back(Begin);
        }
    }
    return Shape;
}

}  // namespace

std::expected<Outline, Fault> ReadOutline(const GlyphTables &Tables, GlyphId Glyph)
{
    Points               Gathered;
    std::vector<GlyphId> Within;
    if (const auto Failed = ReadInto(Tables, Glyph, Gathered, Within)) {
        if (Failed->Glyph == Glyph) return std::unexpected(Fault{std::format("glyf: glyph {} {}", Glyph, Failed->What)});
        return std::unexpected(
            Fault{std::format("glyf: glyph {}: its component, glyph {}, {}", Glyph, Failed->Glyph, Failed->What)});
    }
    return Trace(Gathered);
}

}  // namespace typecase
