/* specks.h — the little things that fly: sparks from the fire and the staff,
 * steam from the pot, drops thrown up by a splash. A speck has a place and a
 * speed in pixels, falls or rises, and fades as its time runs out. */
#ifndef WIZARD_SPECKS_H
#define WIZARD_SPECKS_H

#include <cstdint>
#include <vector>

#include "canvas.h"

namespace wizard {

enum class SpeckKind : std::uint8_t { Spark, Steam, Drop };

struct Speck {
    float         X, Y;           // pixels
    float         Vx, Vy;         // pixels a second
    float         Gravity;        // added to Vy every second: a drop falls, steam climbs
    std::uint32_t LifeMs, AgeMs;
    Color         Ink;
    SpeckKind     Kind;
};

class Specks {
public:
    void Add(const Speck &One);
    void Step(std::uint32_t ElapsedMs);
    void Draw(Canvas &Out) const;
    std::size_t Count() const noexcept { return All.size(); }

private:
    std::vector<Speck> All;
};

}  // namespace wizard

#endif
