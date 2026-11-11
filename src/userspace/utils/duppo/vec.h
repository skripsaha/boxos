/* vec.h — the small arithmetic Duppo's motion and picture share: a point on
 * the glass, a turn in space, the smooth step, and the dice. The unit is the
 * half-block pixel: a cell across, half a cell down, so a unit is square on
 * the glass. In space X goes right, Y down and Z toward the person. */
#ifndef DUPPO_VEC_H
#define DUPPO_VEC_H

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace duppo {

inline constexpr float Pi = 3.14159265f;

struct Vec {
    float X = 0.0f;
    float Y = 0.0f;
};

inline Vec operator+(Vec A, Vec B) { return {A.X + B.X, A.Y + B.Y}; }
inline Vec operator-(Vec A, Vec B) { return {A.X - B.X, A.Y - B.Y}; }
inline Vec operator*(Vec A, float By) { return {A.X * By, A.Y * By}; }
inline Vec operator/(Vec A, float By) { return {A.X / By, A.Y / By}; }

inline float Dot(Vec A, Vec B) { return A.X * B.X + A.Y * B.Y; }
inline float Length(Vec A) { return std::sqrt(Dot(A, A)); }

inline Vec Normalised(Vec A)
{
    float Long = Length(A);
    return Long > 1e-6f ? A / Long : Vec{};
}

inline Vec Limited(Vec A, float Most)
{
    float Long = Length(A);
    return Long > Most ? A * (Most / Long) : A;
}

struct Vec3 {
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

/* A turn in space: yaw about the upright (a positive yaw turns the front to
 * the right), pitch (a positive pitch turns it down), then roll in the plane
 * of the glass. Turned takes a vector of the body into the view, Undone back. */
struct Rotation {
    float M[3][3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};

    static Rotation Of(float Yaw, float Pitch, float Roll)
    {
        const float Cy = std::cos(Yaw), Sy = std::sin(Yaw), Cp = std::cos(Pitch), Sp = std::sin(Pitch);
        const float Cr = std::cos(Roll), Sr = std::sin(Roll);
        const float Row0[3] = {Cy, 0.0f, Sy};
        const float Row1[3] = {-Sp * Sy, Cp, Sp * Cy};
        const float Row2[3] = {-Cp * Sy, -Sp, Cp * Cy};
        Rotation Turn;
        for (int At = 0; At < 3; At++) {
            Turn.M[0][At] = Cr * Row0[At] - Sr * Row1[At];
            Turn.M[1][At] = Sr * Row0[At] + Cr * Row1[At];
            Turn.M[2][At] = Row2[At];
        }
        return Turn;
    }

    Vec3 Turned(Vec3 V) const
    {
        return {M[0][0] * V.X + M[0][1] * V.Y + M[0][2] * V.Z, M[1][0] * V.X + M[1][1] * V.Y + M[1][2] * V.Z,
                M[2][0] * V.X + M[2][1] * V.Y + M[2][2] * V.Z};
    }

    Vec3 Undone(Vec3 V) const
    {
        return {M[0][0] * V.X + M[1][0] * V.Y + M[2][0] * V.Z, M[0][1] * V.X + M[1][1] * V.Y + M[2][1] * V.Z,
                M[0][2] * V.X + M[1][2] * V.Y + M[2][2] * V.Z};
    }
};

inline float Smooth(float Share)
{
    Share = std::clamp(Share, 0.0f, 1.0f);
    return Share * Share * (3.0f - 2.0f * Share);
}

class Random {
public:
    void Seed(std::uint64_t Value) { State = Value | 1; }

    float Unit()
    {
        State ^= State << 13;
        State ^= State >> 7;
        State ^= State << 17;
        return static_cast<float>(State >> 40) / static_cast<float>(1ull << 24);
    }

    float Between(float Low, float High) { return Low + (High - Low) * Unit(); }
    bool  Chance(float Share) { return Unit() < Share; }

private:
    std::uint64_t State = 1;
};

}  // namespace duppo

#endif
