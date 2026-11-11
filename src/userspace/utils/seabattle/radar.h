/* radar.h — a radar screen: a dark disc with its rings and cross hairs, a
 * beam sweeping round with a fading tail, and echoes that light as it passes. */
#ifndef SEABATTLE_RADAR_H
#define SEABATTLE_RADAR_H

#include <cstdint>

#include "canvas.h"

namespace seabattle {

// A radar of Radius pixels about (X, Y), in Tone; Seed places its echoes, and Ms turns the beam.
void PaintRadar(Canvas &Out, float X, float Y, float Radius, Color Tone, std::uint64_t Ms, std::uint32_t Seed);

}  // namespace seabattle

#endif
