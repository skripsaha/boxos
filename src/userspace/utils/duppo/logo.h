/* logo.h — Duppo as the vector master draws it (duppo-bubble.svg): seven
 * discs, each shaded by a radial gradient on its own affine map. The numbers
 * are the SVG's, in its units; Logo::*Color gives the colour of a point of
 * the logo in those units, and nothing here knows of the glass. */
#ifndef DUPPO_LOGO_H
#define DUPPO_LOGO_H

#include "painter/core/color.h"

#include "vec.h"

namespace duppo {

using Color = painter::core::Color;

/* The colours Duppo is drawn in, worked from the three the person sets: the
 * deep plum and the lip round the eye follow the body, the ring's shaded edge
 * follows the ring, and the pupil's glow and rim are lighter shades of it. At
 * the defaults they are the SVG's own. Haze is the dark a little bubble is
 * mixed with where it lies behind the ball. */
struct Palette {
    Color Light, Deep, Lip;
    Color Ring, RingEdge;
    Color Glow, Core, PupilEdge;
    Color Haze;

    static Palette Make(Color Body, Color Ring, Color Pupil);
};

struct Ball {
    Vec   Center;
    float Radius;
};

namespace Logo {

inline constexpr int MoonCount = 3;

inline constexpr Ball Body{{34.510509f, 28.268843f}, 17.16255f};
inline constexpr Ball Socket{{33.996025f, 25.892982f}, 11.508257f};
inline constexpr Ball White{{33.996025f, 25.892982f}, 10.013677f};
inline constexpr Vec  PupilCenter{33.771843f, 24.676687f};
inline constexpr Vec  PupilReach{7.8465376f, 7.9419012f};

/* The medium bubble on the lower right edge, the small one up left, the tiny
 * one to the right: the order they are kept in, so fewer bubbles drop the tiny
 * one first. */
inline constexpr Ball Moons[MoonCount] = {
    {{45.021469f, 42.663223f}, 5.1373868f},
    {{16.983208f, 15.099807f}, 3.5669854f},
    {{55.582016f, 23.11713f}, 2.4925005f},
};

/* How far the logo reaches from the body's centre, bubbles included. */
inline constexpr float ReachLeft  = 21.2f;
inline constexpr float ReachRight = 23.6f;
inline constexpr float ReachUp    = 17.2f;
inline constexpr float ReachDown  = 19.6f;

/* How far the pupil can slide in the white and stay inside it. */
inline constexpr float GazeReach = 1.8f;

Color BodyColor(const Palette &Hues, Vec At);
Color SocketColor(const Palette &Hues, Vec At);
Color WhiteColor(const Palette &Hues, Vec At);
Color PupilColor(const Palette &Hues, Vec At);
Color MoonColor(const Palette &Hues, int Which, Vec At);

}  // namespace Logo

}  // namespace duppo

#endif
