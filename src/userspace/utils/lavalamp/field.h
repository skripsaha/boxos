/* field.h — how much wax there is at each point of a grid laid over the
 * lamp, and how warm it is there: each blob a soft round of wax, the pool a
 * soft floor, added up, so where two come near they flow into one. The level
 * is 1 at the wax's edge, more within it and nothing a third of a radius
 * beyond — a blob reaches no further, so only the points near it are worked
 * out. A face lays the grid as fine as it draws: the pixels' face a point
 * every few pixels, the cells' face a point for each part of a cell. */
#ifndef LAVALAMP_FIELD_H
#define LAVALAMP_FIELD_H

#include <vector>

#include "lamp.h"

namespace lavalamp {

class Field {
public:
    /* Across x Down points, the first at From of the lamp, Step apart. */
    void Lay(int Across, int Down, float FromX, float FromY, float StepX, float StepY);
    void Fill(const Lamp &Of);

    int   Across() const noexcept { return Wide; }
    int   Down() const noexcept { return High; }
    float Level(int X, int Y) const noexcept { return Levels[static_cast<std::size_t>(Y) * Wide + X]; }
    /* How warm the wax is there, 0 to 1: the blobs' warmth, each by how much
     * of the wax there is its. */
    float Heat(int X, int Y) const noexcept;
    /* Where a point is, in the lamp. */
    float X(int At) const noexcept { return FromX + static_cast<float>(At) * StepX; }
    float Y(int At) const noexcept { return FromY + static_cast<float>(At) * StepY; }

private:
    void AddBlob(const Blob &One);
    void AddPool(const Lamp &Of);

    int                Wide = 0, High = 0;
    float              FromX = 0, FromY = 0, StepX = 1, StepY = 1;
    std::vector<float> Levels, Warmth;
};

}  // namespace lavalamp

#endif
