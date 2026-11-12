/* figure.h — the whale as the picture sees it: given how the body lies, what
 * colour it has at any one pixel, if it is there at all. The body is the
 * spine's segments each a tube whose thickness follows the whale's shape,
 * shaded across by how far its side is from the belly; the fins and the
 * flukes are leaves laid beside it. */
#ifndef WHALE_FIGURE_H
#define WHALE_FIGURE_H

#include <array>
#include <cstdint>

#include "painter/core/color.h"

#include "swim.h"

namespace whale {

/* The dark of the water in the whale's edge. */
inline constexpr painter::core::Color Outline = painter::core::Color::Rgb(0x07, 0x12, 0x30);

/* The whale's colours, all worked from two the person names: the back and the
 * belly. The blue whale is the default of both. */
inline constexpr painter::core::Color DefaultBack  = painter::core::Color::Rgb(0x1E, 0x4A, 0x98);
inline constexpr painter::core::Color DefaultBelly = painter::core::Color::Rgb(0xC4, 0xDE, 0xF2);

struct Hues {
    painter::core::Color BackTop, Back, Mottle, Flank, Lower, Belly, Groove, Fluke, Fin;
};

Hues Dress(painter::core::Color Back, painter::core::Color Belly);

enum class Part : std::uint8_t { None, Body, Fin, Tail };

struct Pixel {
    painter::core::Color Ink;
    Part                 Kind = Part::None;
};

class Figure {
public:
    Figure();

    void Pose(const Swimmer &Whale);
    void Wear(const Hues &Colours) { Ink = Colours; }

    /* What of the whale is at a place, in pixels; Kind is None when nothing. */
    Pixel At(Vec Place) const;

    /* Whether any of the whale can be within a radius of a place: false lets a
     * picture skip the whole neighbourhood. */
    bool Reaches(Vec Centre, float Radius) const;

    /* The box that holds every pixel the whale can reach. */
    Vec Least, Most;

private:
    struct Segment {
        Vec   From;
        Vec   Toward;    /* the unit step to the tail */
        Vec   Side;      /* the unit step across, toward the belly when the whale is upright */
        float Length;
        float Along;     /* 0..1, where the segment starts */
        float Facing;
    };

    struct Leaf {
        Vec                  From, To;
        float                Width;
        painter::core::Color Ink;
    };

    float Radius(float Along) const;
    void  AddLeaf(Vec From, Vec To, float Width, painter::core::Color Ink);
    bool  Within(const Leaf &Shape, Vec Place) const;
    Pixel BodyAt(Vec Place) const;
    painter::core::Color Skin(float Along, float Facing, float Offset, float Radius) const;

    Hues                                   Ink = Dress(DefaultBack, DefaultBelly);
    Segment                                Segments[SpineSegments];
    std::array<float, 65>                  Girths;
    float                                  Length = 0.0f;
    Leaf                                   Pectoral;
    Leaf                                   Tails[3];
    int                                    TailCount = 0;
    Vec                                    Eye;
    float                                  EyeRadius = 0.0f;
    bool                                   EyeShown  = false;
};

}  // namespace whale

#endif
