/* swim.h — where the whale is and how its body lies: a head that wanders on
 * a smooth course and turns away from the edges in wide arcs, the body that
 * follows the head's own path with a wave running down it, the fins, the
 * spout and the bubbles. Nothing of the glass or of BoxOS is in it but the
 * clock's seed; the picture is picture.cpp's.
 *
 * The unit is a pixel of the half-block picture: a cell across, half a cell
 * down, so a pixel is square on the glass. */
#ifndef WHALE_SWIM_H
#define WHALE_SWIM_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace whale {

struct Vec {
    float X = 0.0f;
    float Y = 0.0f;
};

inline Vec operator+(Vec A, Vec B) { return {A.X + B.X, A.Y + B.Y}; }
inline Vec operator-(Vec A, Vec B) { return {A.X - B.X, A.Y - B.Y}; }
inline Vec operator*(Vec A, float By) { return {A.X * By, A.Y * By}; }

inline constexpr int SpineSegments = 24;

/* Half the body's thickness at its widest, in lengths of the whale. */
inline constexpr float WidestHalf = 0.115f;

/* Where along the body, snout 0 to tail 1, the blowhole is. */
inline constexpr float BlowholeAt = 0.24f;

/* A water drop thrown by the spout, or a bubble left behind. */
struct Drop {
    Vec   At;
    Vec   Move;
    float Age   = 0.0f;
    float Life  = 1.0f;
    float Phase = 0.0f;
    bool  Spray = false;
};

class Swimmer {
public:
    /* The glass in pixels, across and down, and the whale's size as a share of its usual. */
    void Begin(int PixelsAcross, int PixelsDown, float Size);
    /* A new size while it swims: the body grows into it or shrinks by the path it walks. */
    void Fit(float Size);
    /* How fast it lives, as a share of the usual, and whether it spouts and leaves bubbles. */
    void Tune(float Speed, bool SpoutsNow, bool BubblesNow);
    void Step(float Seconds);

    /* How thick the body is at a place along it, 0 at the snout to 1 at the
     * tail, as a share of its widest. */
    static float Girth(float Along);

    float             Length = 0.0f;
    Vec               Spine[SpineSegments + 1];    /* snout to tail */
    float             Facing[SpineSegments + 1];   /* 1 belly down going right, -1 going left, 0 on its side */
    float             Beat     = 0.0f;             /* the phase of the tail's stroke, radians */
    float             FinSwing = 0.0f;             /* -1..1 */
    std::vector<Drop> Drops;

private:
    struct Mark {
        Vec   At;
        float Facing;
    };

    void Steer(float Seconds);
    void Shape();
    Mark Behind(float Distance) const;
    void Release(float Seconds);
    void Move(float Seconds);
    void Advance(float Seconds);

    int               Across = 0, Down = 0;
    float             Pace = 1.0f;
    bool              Spouts = true, Bubbles = true;
    float             MinX = 0, MaxX = 0, MinY = 0, MaxY = 0;
    float             Cruise = 0.0f;
    Vec               Head;
    float             Heading = 0.0f;
    float             Course = 0.0f;
    float             ToCourse = 4.0f;
    float             Roll = 1.0f;
    float             Time = 0.0f;
    float             ToSpout = 4.0f;
    float             Spouting = 0.0f;
    float             ToBubble = 0.5f;
    std::vector<Mark> Trail;      /* oldest first, the head last */
    float             TrailLength = 0.0f;
};

}  // namespace whale

#endif
