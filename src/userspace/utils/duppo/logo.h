/* logo.h — Duppo as the vector master draws it: seven discs, their shapes
 * boxos_mascot.h's, each shaded by a radial gradient on its own affine map
 * (duppo-bubble.svg's). The numbers are the master's, in its units;
 * Logo::*Color gives the colour of a point of the logo in those units, and
 * nothing here knows of the glass. */
#ifndef DUPPO_LOGO_H
#define DUPPO_LOGO_H

#include "boxos_mascot.h"
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

inline constexpr int MoonCount = BOXOS_DUPPO_MOONS;

constexpr Ball BallOf(const BoxDuppoDisc &Disc) { return {{Disc.X, Disc.Y}, Disc.Radius}; }

inline constexpr Ball Body   = BallOf(BoxDuppoBody);
inline constexpr Ball Socket = BallOf(BoxDuppoSocket);
inline constexpr Ball White  = BallOf(BoxDuppoWhite);
inline constexpr Vec  PupilCenter{BoxDuppoPupil.X, BoxDuppoPupil.Y};
inline constexpr Vec  PupilReach{BoxDuppoPupil.ReachX, BoxDuppoPupil.ReachY};

/* The medium bubble on the lower right edge, the small one up left, the tiny
 * one to the right, in boxos_mascot.h's order. */
inline constexpr Ball Moons[MoonCount] = {BallOf(BoxDuppoMoons[0]), BallOf(BoxDuppoMoons[1]),
                                          BallOf(BoxDuppoMoons[2])};

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
