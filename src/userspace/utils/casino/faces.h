/* faces.h — the pictures on the reels: a red seven, a BAR, a bell, a star, a
 * cherry and a lemon. Each is described by shapes in a square from -1 to 1,
 * not by pixels, and is drawn at whatever size the glass has room for, nine
 * samples to the pixel, so it is as clean at ten pixels as at forty. */
#ifndef CASINO_FACES_H
#define CASINO_FACES_H

#include <cstdint>
#include <vector>

#include "reels.h"

namespace casino {

struct Rgba {
    std::uint8_t Red   = 0;
    std::uint8_t Green = 0;
    std::uint8_t Blue  = 0;
    std::uint8_t Cover = 0;    // 0 clear, 255 solid
};

class Faces {
public:
    // Every symbol drawn Edge pixels square.
    explicit Faces(int Edge);

    int  Edge() const { return Side; }
    Rgba At(Symbol Which, int Across, int Down) const;

private:
    int               Side;
    std::vector<Rgba> Images;
};

}  // namespace casino

#endif
