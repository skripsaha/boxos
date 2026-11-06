#ifndef TUMBLEWEED_H
#define TUMBLEWEED_H

#include <cstdint>
#include <vector>

#include "painter/core/color.h"
#include "painter/tui/frame.h"
#include "painter/tui/stage.h"

using TumbleColor = painter::core::Color;

enum class TumbleWeather : std::uint8_t { Clear, Wind, Dust, Rain, Storm };
enum class TumbleSeason : std::uint8_t { Spring, Summer, Autumn, Winter };
enum class TumbleLight : std::uint8_t { Clock, Day, Dusk, Night };

const char *TumbleWeatherName(TumbleWeather Value);
const char *TumbleSeasonName(TumbleSeason Value);
const char *TumbleLightName(TumbleLight Value);

struct TumbleDice {
    std::uint64_t State;

    explicit TumbleDice(std::uint64_t Seed);
    std::uint64_t Next();
    float Unit();
    float Between(float Low, float High);
    int Count(int Low, int High);
    bool Chance(float Probability);
};

struct TumbleCactus {
    int X = 0;
    int Height = 0;
    bool Arms = false;
    int Layer = 1;
    TumbleColor Ink;
};

struct TumbleRock {
    int X = 0;
    int Width = 0;
    int Height = 0;
    int Layer = 1;
    TumbleColor Ink;
};

struct TumbleRuin {
    int X = 0;
    int Height = 0;
    int Layer = 1;
    TumbleColor Ink;
};

struct TumbleWeed {
    float X = 0;
    float Speed = 0;
    float Spin = 0;
    int Layer = 1;
    TumbleColor Ink;
};

struct TumbleBird {
    float X = 0;
    float Y = 0;
    float Speed = 0;
    float Phase = 0;
    int Layer = 0;
    TumbleColor Ink;
};

struct TumbleDust {
    float X = 0;
    float Y = 0;
    float Speed = 0;
    float Life = 0;
    TumbleColor Ink;
};

struct TumbleWorld {
    int Width = 0;
    int Height = 0;
    int ViewWidth = 0;
    int ViewHeight = 0;
    std::vector<int> Floor;
    std::vector<int> Mesa;
    std::vector<TumbleCactus> Cacti;
    std::vector<TumbleRock> Rocks;
    std::vector<TumbleRuin> Ruins;
    std::vector<TumbleWeed> Weeds;
    std::vector<TumbleBird> Birds;
    std::vector<TumbleDust> Dusts;
    TumbleDice Dice;
    TumbleWeather Weather = TumbleWeather::Clear;
    TumbleSeason Season = TumbleSeason::Summer;
    TumbleLight Light = TumbleLight::Day;
    float Time = 0;
    float CameraX = 0;
    float CameraY = 0;
    float GoalX = 0;
    float GoalY = 0;
    bool Sunbeam = false;
    bool ShowKeys = true;
    int Hour = 12;
    int Minute = 0;
    float ClockRead = -1;
    unsigned Spawned = 0;

    TumbleWorld(int Columns, int Rows);

    int Ground(int X) const;
    int DistantGround(int X) const;
    int MaxX() const;
    int MaxY() const;
    int Sun() const;
    bool Night() const;
    TumbleColor Shade(TumbleColor Ink, int Amount) const;
    TumbleColor SkyAt(int WorldY, int Sunlight) const;
    TumbleColor Sand() const;

    void Step(float Seconds);
    void Move(int Dx, int Dy);
    void CycleWeather();
    void CycleSeason();
    void CycleLight();
    void AddWeed(float X);
    void AddCactus(float X);
    void AddBird(float X, float Y);

private:
    int GroundHeight(int X) const;
    TumbleColor SceneInk();
    void MakeGround();
    void MakeScene();
};

void TumblePaint(const TumbleWorld &World, painter::tui::Frame &Canvas);

#endif
