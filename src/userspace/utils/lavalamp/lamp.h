/* lamp.h — the wax and how it moves, with no screen in it. The lamp is 1 tall
 * and Aspect wide, its top at 0; a blob of wax warms in the pool at the
 * bottom where the bulb is, rises while it is warm, cools under the cap and
 * sinks back, stretched along the way it goes and breathing a little, and
 * each at its own pace so no two keep step. The faces (pixels.h, cells.h)
 * show it through the field it makes (field.h). */
#ifndef LAVALAMP_LAMP_H
#define LAVALAMP_LAMP_H

#include <cstdint>
#include <vector>

#include "composition.h"

namespace lavalamp {

struct Blob {
    float X = 0, Y = 0;          // its middle
    float Vx = 0, Vy = 0;        // a second, of the lamp's height
    float Heat   = 0;            // 0 cold, 1 as warm as the pool
    float Warms  = 5;            // seconds it takes to warm or cool, its own
    float Lift   = 1;            // how much warmth lifts it, its own
    float Size   = 1;            // of the lamp's blob size, its own
    float Phase  = 0;            // where its breathing and its wandering are
    float Across = 0, Down = 0;  // its radii now, of the lamp's height
};

class Lamp {
public:
    Lamp(float Aspect, const Composition &Made);

    /* The glass's new shape: the wax kept inside it. */
    void Reshape(float Aspect);
    /* As many blobs as the composition says, as large: new ones rise out of
     * the pool, the last ones go. */
    void Tune(const Composition &Made);
    /* Seconds gone by, at the composition's flow. */
    void Advance(float Seconds);

    const std::vector<Blob> &Blobs() const noexcept { return Wax; }
    float                    Aspect() const noexcept { return Wide; }
    /* The pool's top at X: the wax lying on the bottom, a wave on it. */
    float                    PoolTop(float X) const noexcept;
    /* The blob of the lamp's size, of its height: what a radius means. */
    float                    BlobSize() const noexcept { return Radius; }

private:
    void  Seed(Blob &One, bool FromPool);
    void  Shape(Blob &One) const;
    void  Move(Blob &One, float Seconds);
    float Roll(float Least, float Most);

    float             Wide;
    float             Radius = 0.095f;
    float             Time   = 0;
    float             Flow   = 1;
    std::vector<Blob> Wax;
    std::uint64_t     State;
};

}  // namespace lavalamp

#endif
