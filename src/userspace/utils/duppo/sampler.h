/* sampler.h — Duppo at one instant, and the colour of any point of the glass,
 * in glass units: the ball with its eye laid on it and turned with it, so the
 * eye narrows toward the edge and goes round it; the little bubbles before or
 * behind the ball by their depth; every point shaded by the logo's own
 * gradients. A colour stands for a sample Sub wide and tall — half a unit by
 * one for a cell's sub-pixel, a pixel's own size on a framebuffer — and an
 * edge is softened over it by the share each disc takes; at the figure's edge
 * that share is an opacity, for what lies beyond is not known. The cells
 * (picture.h) and the pixels (pixels.h) are drawn by it alike. */
#ifndef DUPPO_SAMPLER_H
#define DUPPO_SAMPLER_H

#include "face.h"
#include "flight.h"
#include "logo.h"
#include "orbit.h"
#include "spin.h"

namespace duppo {

class Sampler {
public:
    /* Whole: every edge whole or none, as on a text page or when the person
     * set the edge crisp. */
    Sampler(const Flight &Body, const Face &Eye, const Spin &Turning, const Orbit &Moons, const Palette &Hues, bool Whole,
            Vec Sub);

    /* The colour at Point, translucent where Duppo covers part of the sample
     * and see-through where it covers none, and whether Duppo is there at
     * all. */
    Color At(Vec Point, bool &Reached) const;

private:
    /* A disc that covers the sample, and how near the person it lies there:
     * Which is -1 for the ball and the bubble's number for a bubble, Local the
     * sample in that disc's own logo units. */
    struct Layer {
        float Depth;
        float Share;
        int   Which;
        Vec   Local;
    };

    float Cover(float Far, Vec Toward) const;
    Color Sphere(Vec Local, float Share, Color Under) const;
    Color Eyes(Vec Local, Vec Seen, Color Under) const;
    Color Pupil(Vec Tex, float Scale, Color Under) const;
    Color Lids(Vec Tex, Vec Seen, float SocketFar, float Scale, Color Under) const;
    Color Little(const Layer &Hit, Color Under) const;

    const Flight  &Body;
    const Face    &Eye;
    const Spin    &Turning;
    const Orbit   &Moons;
    const Palette &Hues;
    bool           Whole;
    Vec            Sub;
    float          Near;   /* how far beyond an edge a sample may lie and still be touched by it */
};

/* How far Duppo reaches, in glass units: the body's bounds, its squash
 * included, and every bubble's. */
struct Reach {
    float Left, Top, Right, Bottom;
};
Reach ReachOf(const Flight &Body, const Orbit &Moons);

}  // namespace duppo

#endif
