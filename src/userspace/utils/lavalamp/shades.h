/* shades.h — the lamp's colours, worked out from a composition: the liquid
 * behind the wax, dark under the cap and lit by the bulb below, the glass
 * curving away at its sides with light lying on it; and the wax — the rim's
 * colour where it is cool, the wax's where it is warm, brighter deep inside,
 * lit from above as a round thing is, the bulb's light caught along its
 * underside, and its glow on the liquid round it. Both faces colour with it,
 * so the cells show the same lamp the pixels do. Colours are sRGB's numbers,
 * 0 to 1. */
#ifndef LAVALAMP_SHADES_H
#define LAVALAMP_SHADES_H

#include "composition.h"

namespace lavalamp {

struct Rgb {
    float R = 0, G = 0, B = 0;
};

inline Rgb Mix(Rgb A, Rgb B, float T)
{
    return {A.R + (B.R - A.R) * T, A.G + (B.G - A.G) * T, A.B + (B.B - A.B) * T};
}
inline Rgb Times(Rgb A, float By)
{
    return {A.R * By, A.G * By, A.B * By};
}
inline Rgb Plus(Rgb A, Rgb B)
{
    return {A.R + B.R, A.G + B.G, A.B + B.B};
}

/* How a point of the wax is lit, from how steeply its level falls there: a
 * blob is round, steep at its edge and flat on top — its surface rises as
 * Level / (1 + Level), which flattens the deep middle of the wax. */
struct Light {
    float Lit = 1;     // the light from above, times the colour
    float Shine = 0;   // the bright spot of the light on it
    float Under = 0;   // the bulb's light along its underside
};

/* Slope: how much the level falls a pixel, times SlopeOf the lamp's blob
 * radius in pixels — the same at any size of the screen. */
inline constexpr float SlopeOf = 1.64f;
Light LightOf(float Level, float SlopeX, float SlopeY);

class Shades {
public:
    explicit Shades(const Composition &Made);

    /* The liquid at height Y, 0 at the cap and 1 at the bottom. */
    Rgb   Liquid(float Y) const;
    /* How much light the glass lets through U of the way across: less where it
     * curves away. */
    float Round(float U) const;
    /* The bulb's light in the liquid — as much as Across times Up — and its
     * colour. */
    float BulbAcross(float U) const;
    float BulbUp(float Y) const;
    Rgb   BulbLight() const noexcept { return Lamp; }
    /* The light on the glass, laid over all. */
    float Sheen(float U) const;
    Rgb   Behind(float U, float Y) const { return Plus(Times(Liquid(Y), Round(U)), Times(Lamp, BulbAcross(U) * BulbUp(Y))); }
    /* The wax at a point: as warm as Heat, Level deep, lit as Lit says, at
     * height Y. */
    Rgb   Wax(float Heat, float Level, const Light &Lit, float Y) const;
    /* A pixel: the wax and its glow over what is behind it — Edge, how much
     * the level changes across a pixel, softens its edge to that pixel. */
    Rgb Over(Rgb Back, Rgb Waxen, float Level, float Edge) const
    {
        const float Near  = Level < 1.0f ? Level : 1.0f;
        const float Halo  = Glow * 0.62f * Near * Near * (3.0f - 2.0f * Near);
        float       Cover = 0.5f + (Level - 1.0f) / Edge;
        Cover             = Cover < 0.0f ? 0.0f : Cover > 1.0f ? 1.0f : Cover;
        return Mix(Mix(Back, Times(Waxen, 0.8f), Halo), Waxen, Cover);
    }

private:
    Rgb   Hot, Cool, Bright, Deep, Lamp;
    float Glow;
};

}  // namespace lavalamp

#endif
