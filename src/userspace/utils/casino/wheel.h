/* wheel.h — the roulette wheel: its pockets in the order of a real single-zero
 * wheel, and the flight of the ball.
 *
 * The number is drawn first, fairly, by the caller; the flight is then worked
 * out backwards from the pocket it must end in. The ball runs the rim track
 * against the wheel, slowing, leaves it, rolls down the cone, and hops from
 * fret to fret on bounces that are smaller each time, until it lies in the
 * pocket. Every part of that is a plain motion: nothing jumps. */
#ifndef CASINO_WHEEL_H
#define CASINO_WHEEL_H

#include <cstdint>
#include <string_view>

#include "luck.h"

namespace casino {

inline constexpr int Pockets = 37;

enum class Hue : std::uint8_t { Green, Red, Black };

// The number in the pocket Slot places clockwise from the zero.
int  NumberAt(int Slot);
int  SlotOf(int Number);
Hue  HueOf(int Number);
std::string_view HueName(Hue Of);

// Where everything is at an instant. Angles turn clockwise from the top of
// the glass; radii are fractions of the wheel's outer radius.
struct Flight {
    float WheelAngle  = 0;
    float BallAngle   = 0;
    float BallRadius  = 0;
    float Lift        = 0;    // 0 on the surface, 1 at the top of a bounce
    bool  OnTrack     = true; // still running the rim, not yet fallen to the cone
    bool  Landed      = false;
};

class Spin {
public:
    // A spin that ends with the ball in the pocket of Number.
    Spin(int Number, Luck &Chance);

    // The flight Seconds after the ball was thrown, at the pace of one.
    Flight At(float Seconds) const;
    // When the ball has landed, and when the wheel has stopped.
    float LandsAt() const { return TrackSeconds + DropSeconds; }
    float EndsAt() const { return StopSeconds; }
    int   Result() const { return Number; }

private:
    float WheelAt(float Seconds) const;
    float Rattle(float Along) const;
    float Hop(float Along) const;

    int   Number;
    float TrackSeconds;     // the ball on the rim
    float DropSeconds;      // from leaving it to lying in the pocket
    float StopSeconds;      // the wheel's last turn
    float WheelSpeed;       // radians a second at the start
    float TrackSpeed;       // the ball's at the start, against the wheel
    float DropSpeed;        // and as it leaves the rim
    float DropReach;        // the way it still has to go round the wheel
    float PocketAngle;      // where its pocket stands on the wheel
    float Kick[6];          // how far off the pocket each bounce lands
};

}  // namespace casino

#endif
