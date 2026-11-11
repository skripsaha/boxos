/* art.cpp — the room, the wizard and the cauldron, in pixels. */

#include "art.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace wizard {

namespace {

namespace core = painter::core;

constexpr std::array<std::string_view, WizardHeight> Sprite = {
    "............hh..............", "...........hHh..............", "..........hHHhh.............",
    "..........hHyHHh............", ".........hHHHHHhh...........", ".........hHHHHHHHh..........",
    "........hHHHHHHHHHh.........", "........hHHHHHHHHHHh........", ".......hHHHHHHHHHHHHh.......",
    ".......hbbbbbbbbbbbbbh......", "....hhhhhhhhhhhhhhhhhhhhh...", ".....hhhhhhhhhhhhhhhhhhh....",
    "........wWWWsssssWWWw.......", "........wsseesssseesw.......", "........wssSsssssSssw.......",
    ".......wwsssssnnsssswww.....", ".......wwwwsssnnssswwww.....", "......wwwwwwwmmmwwwwwwww....",
    "..ss..wwwwwwwwwwwwwwwwww....", "..ss.rwwwwwwWwwwwwwwwWwwr...", "...rrrwwwwwWwwwwwWwwwwwwrr..",
    "..rrrrwwwwwwwwwwwwwWwwwwrr..", "..rrrrrwwwwwwWwwwwwwwwwrrr..", "..rrrrrrwwwwwwwwwwwWwwwrrr..",
    "...rrrrrrwwwwWwwwwwwwwrrrr..", ".....rrrrrwwwwwwwwwwwrrrrr..", ".....rrrrrrwwwwwWwwwrrrrrr..",
    ".....rrrrrrrwwwwwwwwrrrrrrr.", "....rrrrrrrrrwwwwwwrrrrrrrr.", "....rrrrrrrrrrwWwwrrrrrrrrr.",
    "...rrrrrrrrrrrrwwrrrrrrrrrr.", "...rrrrrrrrrrrrrrrrrrrrrrrr.", "..rrrrrrrrrrrrrrrrrrrrrrrrrr",
    "..RRRRRRRRRRRRRRRRRRRRRRRRRR",
};

constexpr bool SpriteIsWhole()
{
    for (std::string_view Row : Sprite)
        if (Row.size() != 28) return false;
    return true;
}
static_assert(SpriteIsWhole(), "every row of the wizard is 28 pixels wide");

constexpr std::int32_t StaffX = 28;
constexpr std::int32_t BeardFrom = 18;   // the rows that sway: the beard, and from HemFrom the robe's foot
constexpr std::int32_t HemFrom = 29;

Color Rgb(std::uint8_t Red, std::uint8_t Green, std::uint8_t Blue) noexcept
{
    return Color::Rgb(Red, Green, Blue);
}

Color Tone(char Mark) noexcept
{
    switch (Mark) {
    case 'h': return Rgb(0x2B, 0x2F, 0x7A);
    case 'H': return Rgb(0x46, 0x51, 0xB8);
    case 'b': return Rgb(0xD8, 0xA9, 0x3A);
    case 'y': return Rgb(0xFF, 0xE0, 0x60);
    case 'w': return Rgb(0xF0, 0xF0, 0xF4);
    case 'W': return Rgb(0xB9, 0xBD, 0xCC);
    case 's': return Rgb(0xF0, 0xC0, 0x9A);
    case 'S': return Rgb(0xC8, 0x94, 0x6E);
    case 'e': return Rgb(0x18, 0x18, 0x18);
    case 'n': return Rgb(0xE0, 0x8C, 0x84);
    case 'm': return Rgb(0xA0, 0x40, 0x40);
    case 'r': return Rgb(0x5A, 0x3C, 0x9C);
    default:  return Rgb(0x3C, 0x26, 0x70);
    }
}

// A sine of a time, in 1024ths: -1024..1024, by the parabola that fits it.
std::int32_t Wave(std::uint64_t Ms, std::uint32_t PeriodMs, std::uint32_t Phase) noexcept
{
    std::int32_t Place = static_cast<std::int32_t>(((Ms % PeriodMs) * 1024u / PeriodMs + Phase) & 1023u);
    const bool Below = Place >= 512;
    if (Below) Place -= 512;
    const std::int32_t Height = Place * (512 - Place) / 64;
    return Below ? -Height : Height;
}

std::uint32_t Hash(std::uint32_t Value) noexcept
{
    Value ^= Value >> 16;
    Value *= 0x7FEB352Du;
    Value ^= Value >> 15;
    Value *= 0x846CA68Bu;
    return Value ^ (Value >> 16);
}

bool Beard(char Mark) noexcept
{
    return Mark == 'w' || Mark == 'W';
}

// How far the row sways, in pixels of the picture.
std::int32_t Swing(std::int32_t Row, std::uint64_t Ms) noexcept
{
    if (Row < BeardFrom) return 0;
    const std::int32_t Reach = Row >= HemFrom ? 1 : 0;
    const std::int32_t Beardwards = (Row - BeardFrom + 4) / 6;
    const std::int32_t Wave1 = Wave(Ms, 2600, static_cast<std::uint32_t>(Row) * 40u);
    return (Wave1 * std::max(Reach, Beardwards) + (Wave1 >= 0 ? 512 : -512)) / 1024;
}

Color Brighter(Color Of, std::uint8_t Weight) noexcept
{
    return Color::Mix(Of, Color::Rgb(255, 255, 255), Weight);
}

// A pixel of the Staff column's wood.
constexpr std::uint8_t WoodRed = 0x8A, WoodGreen = 0x5A, WoodBlue = 0x2B;

void DrawStone(const Pen &At, std::int32_t X, std::int32_t Y, const Look &Shown, std::uint64_t Ms,
               std::uint32_t Excitement)
{
    const std::int32_t Pulse = 128 + Wave(Ms, 1800, 0) / 16 + static_cast<std::int32_t>(Excitement) / 2;
    const auto Weight = static_cast<std::uint8_t>(std::clamp(Pulse, 0, 255));
    // The glow it throws: faint further out, brighter near.
    for (std::int32_t Dy = -4; Dy <= 4; Dy++) {
        for (std::int32_t Dx = -5; Dx <= 5; Dx++) {
            const std::int32_t Far = Dx * Dx + Dy * Dy * 2;
            if (Far > 26 || Far < 5) continue;
            At.Veil(X + StaffX + Dx, Y + 4 + Dy, Shown.Glow,
                    static_cast<std::uint8_t>(std::clamp((26 - Far) * Weight / 170, 0, 150)));
        }
    }
    const Color Stone = Color::Mix(Shown.Glow, Color::Rgb(255, 255, 255), Weight / 2);
    const Color Core = Brighter(Shown.Glow, static_cast<std::uint8_t>(Weight));
    At.Block(X + StaffX, Y + 2, 2, 1, Stone);
    At.Block(X + StaffX - 1, Y + 3, 4, 2, Stone);
    At.Block(X + StaffX, Y + 5, 2, 1, Stone);
    At.Block(X + StaffX, Y + 3, 1, 1, Core);
}

void DrawStaff(const Pen &At, std::int32_t X, std::int32_t Y)
{
    const Color Wood = Rgb(WoodRed, WoodGreen, WoodBlue);
    At.Block(X + StaffX, Y + 6, 2, WizardHeight - 6, Wood);
    At.Block(X + StaffX, Y + 6, 1, WizardHeight - 6, Rgb(0xA8, 0x72, 0x3A));
    const Color Skin = Tone('s');
    At.Block(X + 26, Y + 21, 4, 3, Skin);
    At.Block(X + 28, Y + 22, 2, 1, Tone('S'));
}

}  // namespace

void Pen::Dot(std::int32_t X, std::int32_t Y, Color Ink) const
{
    if (X < 0 || Y < 0 || X >= Size.Width || Y >= Size.Height) return;
    for (std::int32_t Dy = 0; Dy < Scale; Dy++)
        for (std::int32_t Dx = 0; Dx < Scale; Dx++)
            Out->PutPixel(core::Point{Origin.X + X * Scale + Dx, Origin.Y + Y * Scale + Dy}, Ink);
}

void Pen::Block(std::int32_t X, std::int32_t Y, std::int32_t Width, std::int32_t Height, Color Ink) const
{
    for (std::int32_t Row = 0; Row < Height; Row++)
        for (std::int32_t Column = 0; Column < Width; Column++) Dot(X + Column, Y + Row, Ink);
}

void Pen::Veil(std::int32_t X, std::int32_t Y, Color Ink, std::uint8_t Weight) const
{
    if (X < 0 || Y < 0 || X >= Size.Width || Y >= Size.Height || Weight == 0) return;
    for (std::int32_t Dy = 0; Dy < Scale; Dy++)
        for (std::int32_t Dx = 0; Dx < Scale; Dx++)
            Out->BlendPixel(core::Point{Origin.X + X * Scale + Dx, Origin.Y + Y * Scale + Dy}, Ink, Weight);
}

void Specks::Add(const Speck &One)
{
    constexpr std::size_t Most = 160;
    if (All.size() < Most) All.push_back(One);
}

void Specks::Step(std::uint32_t ElapsedMs)
{
    for (Speck &One : All) {
        One.AgeMs += ElapsedMs;
        One.X += One.Drift * static_cast<std::int32_t>(ElapsedMs) / 1000;
        One.Y += One.Rise * static_cast<std::int32_t>(ElapsedMs) / 1000;
    }
    std::erase_if(All, [](const Speck &One) { return One.AgeMs >= One.LifeMs; });
}

void Specks::Draw(const Pen &At) const
{
    for (const Speck &One : All) {
        const std::uint32_t Left = One.LifeMs - One.AgeMs;
        const auto          Fade = static_cast<std::uint32_t>(std::min<std::uint64_t>(255, Left * 255ull / (One.LifeMs / 2 + 1)));
        const std::int32_t  X = One.X >> 4, Y = One.Y >> 4;
        if (One.Glows) At.Dot(X, Y, Color::Mix(Color::Rgb(0x20, 0x20, 0x20), One.Ink, static_cast<std::uint8_t>(Fade)));
        else At.Veil(X, Y, One.Ink, static_cast<std::uint8_t>(Fade / 3));
    }
}

core::Point StoneOf(std::int32_t X, std::int32_t Y) noexcept
{
    return core::Point{X + StaffX, Y + 3};
}

core::Point SurfaceOf(std::int32_t X, std::int32_t Y) noexcept
{
    return core::Point{X + 11, Y + 2};
}

void DrawWizard(const Pen &At, std::int32_t X, std::int32_t Y, const Look &Shown, std::uint64_t Ms,
                std::uint32_t Excitement)
{
    DrawStaff(At, X, Y);
    DrawStone(At, X, Y, Shown, Ms, Excitement);

    const bool Blinks = Ms % 3700 < 140;
    for (std::int32_t Row = 0; Row < WizardHeight; Row++) {
        const std::int32_t Shift = Swing(Row, Ms);
        const std::string_view Line = Sprite[static_cast<std::size_t>(Row)];
        for (std::int32_t Column = 0; Column < 28; Column++) {
            char Mark = Line[static_cast<std::size_t>(Column)];
            if (Mark == '.') continue;
            if (Mark == 'e' && Blinks) Mark = 's';
            const std::int32_t Moved = Row >= HemFrom || Beard(Mark) ? Shift : 0;
            At.Dot(X + Column + Moved, Y + Row, Tone(Mark));
        }
    }
}

void DrawCauldron(const Pen &At, std::int32_t X, std::int32_t Y, std::uint32_t Liquid, std::uint32_t Fullness,
                  std::uint64_t Ms)
{
    // The fire first: tongues between the legs, each its own height from the time.
    const Color Flames[3] = {Rgb(0xD8, 0x30, 0x18), Rgb(0xF0, 0x80, 0x20), Rgb(0xFF, 0xDC, 0x60)};
    for (std::int32_t Column = 4; Column < 18; Column++) {
        const std::uint32_t Roll = Hash(static_cast<std::uint32_t>(Column) * 977u + static_cast<std::uint32_t>(Ms / 110));
        const std::int32_t Height = 2 + static_cast<std::int32_t>(Roll % 4u);
        for (std::int32_t Up = 0; Up < Height; Up++)
            At.Dot(X + Column, Y + CauldronHeight - 1 - Up, Flames[std::min<std::int32_t>(2, Up * 3 / Height)]);
    }
    At.Block(X + 3, Y + CauldronHeight - 1, 16, 1, Rgb(0x5A, 0x3A, 0x1E));

    const Color Iron = Rgb(0x34, 0x34, 0x3C);
    const Color Lit = Rgb(0x5C, 0x5C, 0x68);
    for (std::int32_t Row = 3; Row <= 13; Row++) {
        const std::int32_t In = Row < 8 ? 0 : (Row - 7) / 2;
        At.Block(X + 1 + In, Y + Row, CauldronWidth - 2 - 2 * In, 1, Iron);
        At.Dot(X + 3 + In, Y + Row, Lit);
    }
    At.Block(X + 4, Y + 14, 2, 2, Iron);
    At.Block(X + 16, Y + 14, 2, 2, Iron);
    At.Block(X, Y + 3, 1, 3, Iron);
    At.Block(X + CauldronWidth - 1, Y + 3, 1, 3, Iron);
    At.Block(X + 1, Y, CauldronWidth - 2, 2, Lit);
    At.Block(X + 1, Y + 1, CauldronWidth - 2, 1, Iron);

    const Color Water = Rgb(0x40, 0x58, 0x48);
    const Color Surface = Liquid == 0 ? Water : Color::FromRaw(Liquid);
    const std::int32_t Wobble = Wave(Ms, 1400, 0) > 0 ? 1 : 0;
    At.Block(X + 3, Y + 1, CauldronWidth - 6, 2, Color::Mix(Water, Surface, static_cast<std::uint8_t>(Fullness * 60 + 15)));
    At.Block(X + 4 + Wobble, Y + 1, 4, 1, Brighter(Surface, 90));
}

void DrawRoom(const Pen &At, const Look &Shown, std::uint64_t Ms)
{
    At.Block(0, 0, At.Size.Width, At.Size.Height, Shown.Room);

    // The window: a frame, the night in it, a moon and stars that come and go.
    const std::int32_t Left = At.Size.Width - 21, Top = 1;
    const Color Wood = Rgb(WoodRed, WoodGreen, WoodBlue);
    At.Block(Left, Top, 14, 18, Wood);
    At.Block(Left + 1, Top + 1, 12, 16, Rgb(0x16, 0x16, 0x16));
    At.Block(Left + 6, Top + 1, 2, 16, Wood);
    At.Block(Left + 1, Top + 8, 12, 2, Wood);
    At.Block(Left + 2, Top + 3, 3, 3, Rgb(0xF0, 0xEA, 0xC8));
    At.Dot(Left + 3, Top + 3, Rgb(0x16, 0x16, 0x16));
    for (std::uint32_t Star = 0; Star < 7; Star++) {
        const std::uint32_t Roll = Hash(Star * 7919u + 11u);
        if (Wave(Ms + Roll % 3000u, 3000, 0) < -300) continue;
        const std::int32_t Dx = 8 + static_cast<std::int32_t>(Roll % 5u);
        const std::int32_t Dy = 2 + static_cast<std::int32_t>((Roll >> 8) % 14u);
        if (Dy == 8 || Dy == 9) continue;
        At.Dot(Left + Dx, Top + Dy, Rgb(0xFF, 0xFF, 0xE0));
    }

    // The floor, in boards.
    const std::int32_t Floor = At.Size.Height - FloorHeight;
    At.Block(0, Floor, At.Size.Width, FloorHeight, Shown.Floor);
    for (std::int32_t Board = 7; Board < At.Size.Width; Board += 19)
        At.Block(Board, Floor, 1, FloorHeight, Color::Mix(Shown.Floor, Color::Rgb(0, 0, 0), 90));
    At.Block(0, Floor, At.Size.Width, 1, Color::Mix(Shown.Floor, Color::Rgb(255, 255, 255), 40));
}

}  // namespace wizard
