/* room_parts.h — the parts of the room, each in a file of its own: those that
 * do not move are painted once into the room's colours (room_parts.cpp,
 * room_shelves.cpp), those that do are drawn over the lit room every frame
 * (room_life.cpp, room_beasts.cpp). */
#ifndef WIZARD_ROOM_PARTS_H
#define WIZARD_ROOM_PARTS_H

#include "canvas.h"
#include "light.h"
#include "scene.h"

namespace wizard {

// Once, as the colour of each surface.
void PaintWall(Canvas &Out, const Scene &Where, Color Stone);
void PaintBeam(Canvas &Out, const Scene &Where);
void PaintFloor(Canvas &Out, const Scene &Where, Color Boards);
void PaintRug(Canvas &Out, const Scene &Where);
void PaintBooks(Canvas &Out, const Scene &Where);
void PaintWindowFrame(Canvas &Out, const Scene &Where, Color Stone);
void PaintJars(Canvas &Out, const Scene &Where);
void PaintStands(Canvas &Out, const Scene &Where);

// Every frame, over the lit room.
void DrawSky(Canvas &Out, const Scene &Where, const Show &Now);
void DrawHerbs(Canvas &Out, const Scene &Where, const Show &Now, const Lighting &Lights);
void DrawFlames(Canvas &Out, const Scene &Where, const Show &Now);
void DrawCircle(Canvas &Out, const Scene &Where, const Show &Now);
void DrawOwl(Canvas &Out, const Scene &Where, const Show &Now, const Lighting &Lights);
void DrawCat(Canvas &Out, const Scene &Where, const Show &Now, const Lighting &Lights);

// Whether the pixel is the night in the window, and the row the flame of a
// candle (0 the floor's, 1 the books', 2 the sill's) burns at.
bool InGlass(const Box &Window, int X, int Y);
int  FlameRow(const Scene &Where, int Which);

// How strong a lamp burns at this moment, 256 for as it was made: a flame's
// own flicker, `Seed` apart from another's.
int Flicker(std::uint64_t Ms, std::uint32_t Seed, int Base, int Wander);

}  // namespace wizard

#endif
