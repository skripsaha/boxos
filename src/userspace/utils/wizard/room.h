/* room.h — the wizard's tower room: a stone wall with mortar and light falling
 * off it, boards, a rug, shelves of books and jars, a window on the night,
 * candles, hanging herbs, an owl and a cat. What does not move is painted once
 * as the colour of each surface, and every frame it is lit by the lamps of the
 * moment; what moves is drawn over it. */
#ifndef WIZARD_ROOM_H
#define WIZARD_ROOM_H

#include <cstddef>

#include "canvas.h"
#include "light.h"
#include "look.h"
#include "scene.h"

namespace wizard {

enum Lamps : std::size_t { LampFire, LampFloorCandle, LampBookCandle, LampSillCandle, LampMoon, LampCrystal, LampCount };

class Room {
public:
    Room() : Albedo(1, 1) {}

    // The room for Where, in the colours of Shown.
    void Build(const Scene &Where, const Look &Shown);
    // The room as it is at this moment into Out: everything of it but the
    // cauldron and the wizard.
    void Draw(Canvas &Out, const Show &Now);

    // The lamps, for what is drawn over the room.
    const Lighting &Light() const { return Lights; }

private:
    Scene    Place{};
    Canvas   Albedo;
    Lighting Lights;
};

}  // namespace wizard

#endif
