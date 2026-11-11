/* wheel_plate.h — the roulette wheel as a picture, seen from above: the rim,
 * the ball track, the cone, the 37 pockets with their frets, the turret.
 *
 * Most of the wheel does not turn: the rim, the track and the cone look the
 * same at every angle. Where each pixel lies (how far from the middle, at what
 * bearing) and what the still parts of it look like are worked out once, when
 * the wheel's size or colors change, and kept; a frame then only decides, for
 * the pixels of the turret and the pockets, which pocket or spoke is under
 * each. */
#ifndef CASINO_WHEEL_PLATE_H
#define CASINO_WHEEL_PLATE_H

#include <cstdint>
#include <vector>

#include "canvas.h"
#include "look.h"

namespace casino {

class WheelPlate {
public:
    // The wheel of Radius pixels centered at (Across, Down), turned Angle
    // (clockwise from the zero at the top), into Draw. Glow, a pocket's place
    // on the wheel or -1, is lit by Pulse (0 to 1). Revision tells that the
    // colors the person set have changed.
    void Paint(Canvas &Draw, const Look &Shown, std::uint32_t Revision, float Across, float Down, float Radius, float Angle,
               int Glow, float Pulse);

private:
    enum class Part : std::uint8_t { Outside, Still, Spokes, Pockets };

    // One point sampled inside a pixel.
    struct Sample {
        float        Bearing = 0;    // from the top, clockwise, 0 to a turn
        float        Far = 0;        // pixels from the middle
        Color        Fixed;          // what a still part looks like; the wood between spokes
        Color        Alt;            // the spoke's gold; the fret's gold
        Part         Kind = Part::Outside;
        std::uint8_t Depth = 0;      // how far out in the pockets, in steps
    };
    struct Pixel {
        std::int32_t Col = 0;
        std::int32_t Row = 0;
    };

    void  Build(const Look &Shown, float Across, float Down, float Radius);
    Color Seen(const Sample &At, float Angle, int Glow, float Pulse) const;

    std::vector<Pixel>  Pixels;
    std::vector<Sample> Samples;       // Grid * Grid for each pixel
    Color               Shaded[3][8];  // the pockets, by hue and by depth
    int                 Grid = 1;
    float               Key[3]    = {-1, -1, -1};
    std::uint8_t        Hues[37]  = {};     // the hue of the pocket at each place on the wheel
    std::uint32_t       Version   = ~0u;
};

}  // namespace casino

#endif
