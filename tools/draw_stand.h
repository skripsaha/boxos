#ifndef DRAW_STAND_H
#define DRAW_STAND_H

/* draw stand — what its parts share (tools/draw_stand.cpp). */

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#include "boxos_paint.h"
#include "boxos_shape.h"
#include "boxos_stroke.h"

namespace stand {

/* Checks held and failed; a failure is said as it happens. */
struct Tally {
    int Held   = 0;
    int Failed = 0;

    void Check(bool Holds, const std::string &What);
};

/* A small random source, the same run after run. */
std::uint32_t Random();
std::int32_t  Within(std::int32_t Low, std::int32_t High);

/* Room a shape is built in on the stand. */
struct ShapeRoom {
    std::vector<BoxShapeEdge> Edges = std::vector<BoxShapeEdge>(1u << 16);
    BoxShape                  Shape{};

    BoxShape &Begin(const BoxMatrix &M);
};

extern const BoxMatrix Plain;

/* The share of 255 the shape holds of every pixel of a Width x Height canvas,
 * swept as a fill sweeps it, and the area it holds in pixels by its rule. */
struct Coverage {
    std::int32_t              Width = 0, Height = 0;
    std::vector<std::uint8_t> Shares;
    double                    Area = 0;

    std::uint8_t At(std::int32_t X, std::int32_t Y) const { return Shares[(std::size_t)Y * Width + X]; }
};
Coverage CoverageOf(BoxShape &S, std::int32_t Width, std::int32_t Height);

bool Near(double Got, double Want, double Share);

/* Each part of the checks, adding to Count. */
void CheckShapes(Tally &Count);
void CheckStrokes(Tally &Count);
void CheckPaints(Tally &Count);
void CheckEngine(Tally &Count);
void CheckLanes(Tally &Count);
void CheckShadows(Tally &Count);

/* A sheet of everything the engine draws, a PPM at Out, to look at. */
bool WriteSheet(const char *Out);

/* How long each piece of the engine's work takes, every way this processor
 * has: on the host a guide only — drawcheck says it on the machine itself. */
void SaySpeed();

}  // namespace stand

#endif  // DRAW_STAND_H
