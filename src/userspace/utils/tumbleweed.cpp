/* tumbleweed — a living wasteland behind the console glass.
 *
 *   arrows       walk the window through the wide, deep desert
 *   space        call down or dismiss the sunbeam
 *   s            send a tumbleweed across the sand
 *   c            grow a cactus at the window
 *   b            release a bird into the sky
 *   w            change weather: clear, wind, dust, rain, storm
 *   e            change season
 *   n            change light: clock, day, dusk, night
 *   h, ?         show or hide the key line
 *   q, ESC       leave
 *
 * Painter owns the frame and input. The desert owns its world, camera and
 * objects, so the visual model remains independent from the console backend.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "box/clock.h"
#include "box/cpu.h"
#include "box/form.h"
#include "box/print.h"

#include "painter/core/color.h"
#include "painter/tui/stage.h"

BOX_FORM("");

namespace {

using Color = painter::core::Color;
namespace Colors = painter::core::Colors;
using painter::tui::Frame;
using painter::tui::Stage;
using painter::tui::StageError;
using painter::tui::TextStyle;
namespace KeyCodes = painter::tui::KeyCodes;

constexpr std::uint64_t FrameUs = 33333;
constexpr float MostStep = 0.05f;
constexpr float MostGap = 0.25f;
constexpr int LeastCols = 48;
constexpr int LeastRows = 16;

enum class Weather : std::uint8_t { Clear, Wind, Dust, Rain, Storm };
enum class Season : std::uint8_t { Spring, Summer, Autumn, Winter };
enum class LightMode : std::uint8_t { Clock, Day, Dusk, Night };

const char *WeatherName(Weather Value)
{
    switch (Value) {
    case Weather::Clear: return "clear";
    case Weather::Wind: return "wind";
    case Weather::Dust: return "dust";
    case Weather::Rain: return "rain";
    case Weather::Storm: return "storm";
    }
    return "clear";
}

const char *SeasonName(Season Value)
{
    switch (Value) {
    case Season::Spring: return "spring";
    case Season::Summer: return "summer";
    case Season::Autumn: return "autumn";
    case Season::Winter: return "winter";
    }
    return "summer";
}

const char *LightName(LightMode Value)
{
    switch (Value) {
    case LightMode::Clock: return "clock";
    case LightMode::Day: return "day";
    case LightMode::Dusk: return "dusk";
    case LightMode::Night: return "night";
    }
    return "clock";
}

void Say(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_to_screen(Line.c_str());
}

struct DesertDice {
    std::uint64_t State;

    explicit DesertDice(std::uint64_t Seed) : State(Seed ? Seed : 0x9E3779B97F4A7C15ull) {}

    std::uint64_t Next()
    {
        State ^= State << 13;
        State ^= State >> 7;
        State ^= State << 17;
        return State;
    }

    float Unit() { return static_cast<float>(Next() >> 40) / static_cast<float>(1ull << 24); }
    float Between(float Low, float High) { return Low + (High - Low) * Unit(); }
    int Count(int Low, int High) { return Low + static_cast<int>(Next() % static_cast<std::uint64_t>(High - Low + 1)); }
    bool Chance(float Probability) { return Unit() < Probability; }
};

struct Cactus {
    int X = 0;
    int Height = 0;
    bool Arms = false;
    Color Ink;
};

struct Rock {
    int X = 0;
    int Width = 0;
    int Height = 0;
    Color Ink;
};

struct Ruin {
    int X = 0;
    int Height = 0;
    Color Ink;
};

struct Weed {
    float X = 0;
    float Speed = 0;
    float Spin = 0;
    Color Ink;
};

struct Bird {
    float X = 0;
    float Y = 0;
    float Speed = 0;
    float Phase = 0;
    Color Ink;
};

struct Dust {
    float X = 0;
    float Y = 0;
    float Speed = 0;
    float Life = 0;
    Color Ink;
};

struct Desert {
    int Width = 0;
    int Height = 0;
    int ViewWidth = 0;
    int ViewHeight = 0;
    std::vector<int> Floor;
    std::vector<int> Mesa;
    std::vector<Cactus> Cacti;
    std::vector<Rock> Rocks;
    std::vector<Ruin> Ruins;
    std::vector<Weed> Weeds;
    std::vector<Bird> Birds;
    std::vector<Dust> Dusts;
    DesertDice Dice;
    Weather WeatherState = Weather::Clear;
    Season SeasonState = Season::Summer;
    LightMode LightState = LightMode::Clock;
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

    Desert(int Columns, int Rows)
        : Width(std::max(Columns * 3, 120)), Height(std::max(Rows * 2 + 20, 56)),
          ViewWidth(Columns), ViewHeight((Rows - 1) * 2), Dice(cpu_rdtsc() | 1)
    {
        MakeGround();
        MakeScene();
        GoalX = CameraX = static_cast<float>(std::max(0, Width - ViewWidth) / 2);
        GoalY = CameraY = static_cast<float>(std::max(0, Height - ViewHeight) / 3);
    }

    int Ground(int X) const
    {
        if (Floor.empty()) return Height - 6;
        X = std::clamp(X, 0, Width - 1);
        return Floor[static_cast<std::size_t>(X)];
    }

    int MaxX() const { return std::max(0, Width - ViewWidth); }
    int MaxY() const { return std::max(0, Height - ViewHeight); }

    int Sun() const
    {
        switch (LightState) {
        case LightMode::Day: return 256;
        case LightMode::Dusk: return 112;
        case LightMode::Night: return 0;
        case LightMode::Clock: {
            const int Now = Hour * 60 + Minute;
            if (Now >= 7 * 60 && Now < 19 * 60) return 256;
            if (Now >= 5 * 60 + 30 && Now < 7 * 60) return (Now - 330) * 256 / 90;
            if (Now >= 19 * 60 && Now < 20 * 60 + 30) return (1230 - Now) * 256 / 90;
            return 0;
        }
        }
        return 0;
    }

    bool Night() const { return Sun() < 48; }

    Color Shade(Color Ink, int Amount) const
    {
        return Color::Mix(Colors::Black, Ink, static_cast<std::uint8_t>(std::clamp(Amount, 0, 256)));
    }

    Color Sand() const
    {
        switch (SeasonState) {
        case Season::Spring: return Color::Rgb(0xA9, 0x7A, 0x42);
        case Season::Summer: return Color::Rgb(0xD0, 0x91, 0x43);
        case Season::Autumn: return Color::Rgb(0xA8, 0x63, 0x36);
        case Season::Winter: return Color::Rgb(0xB4, 0xA1, 0x83);
        }
        return Color::Rgb(0xD0, 0x91, 0x43);
    }

    int GroundHeight(int X) const
    {
        float Broad = std::sin(X * 0.026f + 1.1f) * 3.0f;
        float Small = std::sin(X * 0.091f + 2.7f) * 1.2f;
        return Height - 7 - static_cast<int>(Broad + Small);
    }

    void MakeGround()
    {
        Floor.resize(static_cast<std::size_t>(Width));
        Mesa.resize(static_cast<std::size_t>(Width));
        for (int X = 0; X < Width; X++) {
            Floor[static_cast<std::size_t>(X)] = GroundHeight(X);
            float Hill = std::sin(X * 0.014f + 0.7f) * 3.0f + std::sin(X * 0.039f) * 1.4f;
            Mesa[static_cast<std::size_t>(X)] = Height / 2 + static_cast<int>(Hill);
        }
    }

    Color SceneInk()
    {
        return Color::Rgb(static_cast<std::uint8_t>(Dice.Count(80, 150)),
                          static_cast<std::uint8_t>(Dice.Count(65, 120)),
                          static_cast<std::uint8_t>(Dice.Count(45, 88)));
    }

    void MakeScene()
    {
        for (int Made = Width / 20; Made > 0; Made--) {
            const int X = Dice.Count(4, Width - 5);
            Cacti.push_back({X, Dice.Count(8, 19), Dice.Chance(0.65f),
                             Dice.Chance(0.5f) ? Color::Rgb(0x49, 0x9B, 0x4B) : Color::Rgb(0x77, 0xA6, 0x45)});
        }
        for (int Made = Width / 24; Made > 0; Made--)
            Rocks.push_back({Dice.Count(3, Width - 8), Dice.Count(4, 11), Dice.Count(2, 6), SceneInk()});
        for (int Made = Width / 90; Made > 0; Made--)
            Ruins.push_back({Dice.Count(8, Width - 10), Dice.Count(8, 18), Color::Rgb(0x78, 0x5A, 0x48)});
        for (int Made = Width / 70; Made > 0; Made--) AddBird(Dice.Between(0, static_cast<float>(Width)), Dice.Between(8, 28));
        for (int Made = Width / 80; Made > 0; Made--) AddWeed(Dice.Between(0, static_cast<float>(Width)));
    }

    void AddWeed(float X)
    {
        if (Weeds.size() >= 80) return;
        const float Direction = Dice.Chance(0.5f) ? 1.0f : -1.0f;
        Weeds.push_back({std::clamp(X, 2.0f, static_cast<float>(Width - 3)),
                         Direction * Dice.Between(2.5f, 8.0f), Dice.Between(0, 6.28f),
                         Color::Rgb(0x9B, 0x72, 0x40)});
        Spawned++;
    }

    void AddCactus(float X)
    {
        if (Cacti.size() >= 100) return;
        Cacti.push_back({std::clamp(static_cast<int>(X), 4, Width - 5), Dice.Count(7, 18), Dice.Chance(0.7f),
                         Color::Rgb(0x5F, 0xA1, 0x4A)});
        Spawned++;
    }

    void AddBird(float X, float Y)
    {
        if (Birds.size() >= 64) return;
        Birds.push_back({X, Y, Dice.Between(4.0f, 10.0f) * (Dice.Chance(0.5f) ? 1.0f : -1.0f), Dice.Between(0, 6.28f),
                         Color::Rgb(0xCE, 0xC6, 0xAE)});
    }

    void Step(float Seconds)
    {
        Time += Seconds;
        if (LightState == LightMode::Clock && (ClockRead < 0 || Time - ClockRead >= 1.0f)) {
            BoxTime Now;
            if (clock_boxtime_local(&Now) == 0) { Hour = Now.hour; Minute = Now.minute; }
            ClockRead = Time;
        }

        float Wind = WeatherState == Weather::Clear ? 0.0f : WeatherState == Weather::Wind ? 2.0f : 4.0f;
        if (WeatherState == Weather::Storm) Wind = 7.0f;
        for (Weed &One : Weeds) {
            One.X += (One.Speed + Wind) * Seconds;
            One.Spin += Seconds * (3.0f + std::abs(One.Speed));
            if (One.X < -6) One.X = static_cast<float>(Width + 5);
            if (One.X > Width + 6) One.X = -5.0f;
        }
        for (Bird &One : Birds) {
            One.X += One.Speed * Seconds;
            One.Y += std::sin(Time * 2.0f + One.Phase) * Seconds * 0.8f;
            if (One.X < -8) One.X = static_cast<float>(Width + 8);
            if (One.X > Width + 8) One.X = -8.0f;
        }
        if (WeatherState == Weather::Dust || WeatherState == Weather::Storm) {
            if (Dusts.size() < 180 && Dice.Chance(Seconds * (WeatherState == Weather::Storm ? 30.0f : 14.0f)))
                Dusts.push_back({CameraX + ViewWidth + 2.0f, CameraY + Dice.Between(8, static_cast<float>(ViewHeight - 4)),
                                 Dice.Between(-8.0f, -2.0f), Dice.Between(0.8f, 2.5f),
                                 Color::Rgb(0xC0, 0x97, 0x62)});
        }
        for (std::size_t At = Dusts.size(); At-- > 0;) {
            Dust &One = Dusts[At];
            One.X += One.Speed * Seconds;
            One.Life -= Seconds;
            if (One.Life <= 0 || One.X < CameraX - 8) Dusts.erase(Dusts.begin() + static_cast<std::ptrdiff_t>(At));
        }
        CameraX += (GoalX - CameraX) * std::min(1.0f, Seconds * 8.0f);
        CameraY += (GoalY - CameraY) * std::min(1.0f, Seconds * 8.0f);
    }

    void Move(int Dx, int Dy)
    {
        GoalX = std::clamp(GoalX + static_cast<float>(Dx), 0.0f, static_cast<float>(MaxX()));
        GoalY = std::clamp(GoalY + static_cast<float>(Dy), 0.0f, static_cast<float>(MaxY()));
    }

    void CycleWeather() { WeatherState = static_cast<Weather>((static_cast<int>(WeatherState) + 1) % 5); }
    void CycleSeason() { SeasonState = static_cast<Season>((static_cast<int>(SeasonState) + 1) % 4); }
    void CycleLight() { LightState = static_cast<LightMode>((static_cast<int>(LightState) + 1) % 4); ClockRead = -1; }
};

void Pixel(Frame &Canvas, const Desert &World, int X, int Y, Color Ink)
{
    Canvas.PutPixel({X - static_cast<int>(World.CameraX), Y - static_cast<int>(World.CameraY)}, Ink);
}

void Line(Frame &Canvas, const Desert &World, int X0, int Y0, int X1, int Y1, Color Ink)
{
    const int Steps = std::max(std::abs(X1 - X0), std::abs(Y1 - Y0));
    for (int At = 0; At <= Steps; At++) {
        const float T = Steps == 0 ? 0.0f : static_cast<float>(At) / Steps;
        Pixel(Canvas, World, static_cast<int>(X0 + (X1 - X0) * T), static_cast<int>(Y0 + (Y1 - Y0) * T), Ink);
    }
}

void Disc(Frame &Canvas, const Desert &World, int X, int Y, int Radius, Color Ink)
{
    for (int Dy = -Radius; Dy <= Radius; Dy++)
        for (int Dx = -Radius; Dx <= Radius; Dx++)
            if (Dx * Dx + Dy * Dy <= Radius * Radius) Pixel(Canvas, World, X + Dx, Y + Dy, Ink);
}

void DrawCactus(Frame &Canvas, const Desert &World, const Cactus &One)
{
    const int Base = World.Ground(One.X);
    const Color Dark = Color::Mix(One.Ink, Colors::Black, 80);
    for (int Rise = 0; Rise < One.Height; Rise++) {
        Pixel(Canvas, World, One.X, Base - Rise, One.Ink);
        if (Rise % 3 == 1) Pixel(Canvas, World, One.X + 1, Base - Rise, Dark);
    }
    if (!One.Arms) return;
    const int Left = One.Height / 2 + 1;
    const int Right = One.Height * 2 / 3;
    Line(Canvas, World, One.X, Base - Left, One.X - 4, Base - Left, One.Ink);
    Line(Canvas, World, One.X - 4, Base - Left, One.X - 4, Base - Left - 4, One.Ink);
    Line(Canvas, World, One.X, Base - Right, One.X + 4, Base - Right, One.Ink);
    Line(Canvas, World, One.X + 4, Base - Right, One.X + 4, Base - Right - 3, One.Ink);
}

void DrawWeed(Frame &Canvas, const Desert &World, const Weed &One)
{
    const int X = static_cast<int>(One.X);
    const int Y = World.Ground(X) - 3;
    const int Reach = 3 + static_cast<int>(std::abs(std::sin(One.Spin)) * 2.0f);
    const Color Light = Color::Mix(One.Ink, Colors::White, 45);
    Line(Canvas, World, X - Reach, Y, X + Reach, Y, One.Ink);
    Line(Canvas, World, X - 2, Y - Reach, X + 2, Y + Reach, Light);
    Line(Canvas, World, X - 2, Y + Reach, X + 2, Y - Reach, One.Ink);
}

void DrawBird(Frame &Canvas, const Desert &World, const Bird &One)
{
    const int X = static_cast<int>(One.X);
    const int Y = static_cast<int>(One.Y);
    const int Wing = static_cast<int>(std::sin(World.Time * 8.0f + One.Phase) * 2.0f);
    Pixel(Canvas, World, X, Y, One.Ink);
    Line(Canvas, World, X - 1, Y, X - 4, Y - 2 - Wing, One.Ink);
    Line(Canvas, World, X + 1, Y, X + 4, Y - 2 + Wing, One.Ink);
}

void DrawRuin(Frame &Canvas, const Desert &World, const Ruin &One)
{
    const int Base = World.Ground(One.X);
    for (int Rise = 0; Rise < One.Height; Rise++) {
        const int Width = std::max(1, 7 - Rise / 4);
        for (int Col = -Width; Col <= Width; Col++)
            if ((Col + Rise) % 4 != 0) Pixel(Canvas, World, One.X + Col, Base - Rise, One.Ink);
    }
}

void DrawRock(Frame &Canvas, const Desert &World, const Rock &One)
{
    const int Base = World.Ground(One.X);
    for (int Row = 0; Row < One.Height; Row++) {
        const int Half = std::max(1, One.Width / 2 - Row / 2);
        for (int Col = -Half; Col <= Half; Col++) Pixel(Canvas, World, One.X + Col, Base - Row, One.Ink);
    }
}

void DrawWorld(Frame &Canvas, Desert &World)
{
    const int Columns = World.ViewWidth;
    const int PixelsHigh = World.ViewHeight;
    std::vector<Color> Pixels(static_cast<std::size_t>(Columns * (PixelsHigh + 2)), Colors::Black);
    const int Sun = World.Sun();
    for (int Y = 0; Y < PixelsHigh + 2; Y++) {
        for (int X = 0; X < Columns; X++) {
            const int WorldX = std::clamp(static_cast<int>(World.CameraX) + X, 0, World.Width - 1);
            const int WorldY = static_cast<int>(World.CameraY) + Y;
            Color Ink;
            if (WorldY >= World.Ground(WorldX)) {
                Ink = World.Shade(World.Sand(), Sun + 30);
            } else if (WorldY >= World.Mesa[static_cast<std::size_t>(WorldX)]) {
                Ink = World.Shade(Color::Rgb(0x65, 0x4E, 0x53), Sun + 15);
            } else if (World.Night()) {
                const int Violet = std::clamp(12 + (WorldY * 12 / std::max(1, World.Height)), 8, 30);
                Ink = Color::Rgb(static_cast<std::uint8_t>(Violet), static_cast<std::uint8_t>(Violet),
                                 static_cast<std::uint8_t>(Violet + 16));
            } else {
                Color Sky = World.SeasonState == Season::Winter ? Color::Rgb(0x93, 0xB2, 0xBF)
                         : Color::Rgb(0xD7, 0x8E, 0x54);
                Ink = World.Shade(Sky, Sun + 50 - WorldY / 8);
            }
            if (World.WeatherState == Weather::Dust || World.WeatherState == Weather::Storm)
                Ink = Color::Mix(Ink, Color::Rgb(0xB7, 0x8B, 0x5D), 55);
            Pixels[static_cast<std::size_t>(Y) * Columns + X] = Ink;
        }
    }
    Canvas.PutPixels(Pixels);

    const int SunX = World.Width / 2;
    const int SunY = 12;
    if (Sun > 0) {
        Disc(Canvas, World, SunX, SunY, 4, World.Shade(Color::Rgb(0xFF, 0xD0, 0x62), Sun + 40));
        if (World.Sunbeam) {
            const int End = World.Ground(static_cast<int>(World.CameraX) + Columns / 2);
            for (int Ray = -4; Ray <= 4; Ray++)
                Line(Canvas, World, SunX + Ray, SunY + 4, static_cast<int>(World.CameraX) + Columns / 2 + Ray * 3,
                     End, Color::Mix(Colors::Amber, Colors::White, 50));
        }
    } else {
        Disc(Canvas, World, SunX, SunY, 3, Color::Rgb(0xA8, 0xB5, 0xD0));
        for (int Star = 0; Star < World.Width / 10; Star++) {
            const int X = static_cast<int>((static_cast<std::uint64_t>(Star) * 7919u) % static_cast<std::uint64_t>(World.Width));
            const int Y = 5 + static_cast<int>((static_cast<std::uint64_t>(Star) * 3571u) % 24u);
            Pixel(Canvas, World, X, Y, Colors::White);
        }
    }

    for (const Ruin &One : World.Ruins) DrawRuin(Canvas, World, One);
    for (const Rock &One : World.Rocks) DrawRock(Canvas, World, One);
    for (const Cactus &One : World.Cacti) DrawCactus(Canvas, World, One);
    for (const Weed &One : World.Weeds) DrawWeed(Canvas, World, One);
    for (const Bird &One : World.Birds) DrawBird(Canvas, World, One);
    for (const Dust &One : World.Dusts) Pixel(Canvas, World, static_cast<int>(One.X), static_cast<int>(One.Y), One.Ink);

    if (World.WeatherState == Weather::Rain || World.WeatherState == Weather::Storm) {
        const int Drops = Columns / 3 + (World.WeatherState == Weather::Storm ? Columns / 3 : 0);
        for (int Drop = 0; Drop < Drops; Drop++) {
            const int X = static_cast<int>((static_cast<std::uint64_t>(Drop * 101 + World.Time * 80) * 17u) % static_cast<std::uint64_t>(Columns));
            const int Y = static_cast<int>((static_cast<std::uint64_t>(Drop * 53 + World.Time * 130) * 13u) % static_cast<std::uint64_t>(std::max(1, PixelsHigh)));
            Pixel(Canvas, World, static_cast<int>(World.CameraX) + X, static_cast<int>(World.CameraY) + Y,
                  Color::Rgb(0x72, 0xA9, 0xC8));
        }
    }
}

void DrawBar(Frame &Canvas, const Desert &World)
{
    const int Row = World.ViewHeight / 2;
    const TextStyle Bar{Color::Rgb(0xEE, 0xEE, 0xEE), Color::Rgb(0x3A, 0x3A, 0x3A)};
    const TextStyle Hint{Color::Rgb(0x8C, 0x8C, 0x8C), Bar.Background};
    Canvas.Fill({0, Row, World.ViewWidth, 1}, ' ', Bar);
    int Col = 1;
    Col += static_cast<int>(Canvas.Write({Col, Row}, "tumbleweed", Bar));
    Col += static_cast<int>(Canvas.Write({Col + 1, Row}, WeatherName(World.WeatherState), Hint)) + 2;
    Col += static_cast<int>(Canvas.Write({Col + 1, Row}, SeasonName(World.SeasonState), Hint)) + 2;
    Col += static_cast<int>(Canvas.Write({Col + 1, Row}, LightName(World.LightState), Hint)) + 2;
    if (World.Sunbeam) Col += static_cast<int>(Canvas.Write({Col + 1, Row}, "beam", Bar)) + 2;
    std::string Count = std::format("{} spawned", World.Spawned);
    if (Col + static_cast<int>(Count.size()) + 2 < World.ViewWidth)
        Col += static_cast<int>(Canvas.Write({Col + 1, Row}, Count, Hint)) + 2;
    if (!World.ShowKeys) return;
    Canvas.Write({Col + 1, Row}, "arrows move  space beam  w weather  e season  n light  s/c/b spawn  h hide  q leave", Hint);
}

bool Draw(Desert &World, Stage &Screen)
{
    Frame &Canvas = Screen.FrameBuffer();
    DrawWorld(Canvas, World);
    DrawBar(Canvas, World);
    if (!Screen.SetCursor({World.ViewWidth - 1, World.ViewHeight / 2})) return false;
    return static_cast<bool>(Screen.Present());
}

bool OnKey(Desert &World, const painter::tui::KeyEvent &Key)
{
    if (Key.IsExtended()) {
        const int StepX = std::max(4, World.ViewWidth / 4);
        const int StepY = std::max(4, World.ViewHeight / 4);
        switch (Key.Scancode) {
        case KeyCodes::Left: World.Move(-StepX, 0); break;
        case KeyCodes::Right: World.Move(StepX, 0); break;
        case KeyCodes::Up: World.Move(0, -StepY); break;
        case KeyCodes::Down: World.Move(0, StepY); break;
        default: break;
        }
        return true;
    }
    switch (Key.Ascii) {
    case ' ': World.Sunbeam = !World.Sunbeam; break;
    case 's':
    case 'S': World.AddWeed(World.CameraX + World.ViewWidth * 0.5f); break;
    case 'c':
    case 'C': World.AddCactus(World.CameraX + World.ViewWidth * 0.5f); break;
    case 'b':
    case 'B': World.AddBird(World.CameraX + World.ViewWidth * 0.5f, World.CameraY + 12); break;
    case 'w':
    case 'W': World.CycleWeather(); break;
    case 'e':
    case 'E': World.CycleSeason(); break;
    case 'n':
    case 'N': World.CycleLight(); break;
    case 'h':
    case 'H':
    case '?': World.ShowKeys = !World.ShowKeys; break;
    case 'q':
    case 'Q':
    case KeyCodes::Escape: return false;
    default: break;
    }
    return true;
}

}

int main()
{
    try {
        std::expected<Stage, StageError> Opened = Stage::Open("tumbleweed", LeastCols, LeastRows);
        if (!Opened) {
            Say(Opened.error().Describe("tumbleweed"));
            return 1;
        }
        Stage Screen = std::move(*Opened);
        Desert World(Screen.Size().Width, Screen.Size().Height);
        const std::uint64_t Began = clock_uptime_us();
        std::uint64_t Last = Began;
        bool Painted = true;
        for (;;) {
            const std::uint64_t Now = clock_uptime_us();
            const float Elapsed = std::min(MostGap, static_cast<float>(Now - Last) / 1e6f);
            for (float Left = Elapsed; Left > 0; Left -= MostStep) World.Step(std::min(MostStep, Left));
            Last = Now;
            if (!(Painted = Draw(World, Screen))) break;

            const std::uint64_t Spent = clock_uptime_us() - Now;
            const painter::tui::Event Event = Screen.Wait(
                Spent >= FrameUs ? 1 : static_cast<std::uint32_t>((FrameUs - Spent) / 1000 + 1));
            if (Event.Kind == painter::tui::EventKind::Gone) break;
            if (Event.Kind == painter::tui::EventKind::Key && !OnKey(World, Event.Key)) break;
        }
        const unsigned Spawned = World.Spawned;
        (void)Screen.Close();
        Say(std::format("[TUMBLEWEED] {} object{} called into the wasteland in {} seconds", Spawned,
                        Spawned == 1 ? " was" : "s were", (clock_uptime_us() - Began) / 1000000));
        return Painted ? 0 : 1;
    } catch (const std::bad_alloc &) {
        Say("There is not enough memory for a wasteland the size of this screen.");
        return 1;
    }
}
