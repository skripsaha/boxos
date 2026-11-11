/* paint_fauna.cpp — the animals: each picture in the colours of its kind and
 * of the season — the hare turning white with the snow, the deer's coat
 * greying, the stag's antlers shed — with the legs of what walks drawn
 * turning with its stride, and the fireflies blinking in the dark. */

#include <algorithm>
#include <cmath>
#include <optional>

#include "paint.h"
#include "sprites.h"

namespace forest {

namespace {

constexpr float Pi = 3.14159265358979323846f;

using Maybe = std::optional<Color>;

int Weighted(float Value) { return static_cast<int>(std::lround(std::clamp(Value, 0.0f, 1.0f) * 256.0f)); }

bool Drifts(Kind Which) { return Which == Kind::Bat || Which == Kind::Butterfly || Which == Kind::Firefly; }

Maybe FoxInk(char Letter)
{
    switch (Letter) {
    case 'b': return Color::Rgb(0xC8, 0x64, 0x2A);
    case 's': return Color::Rgb(0xED, 0xD4, 0xA8);
    case 'w': return Color::Rgb(0xF6, 0xF0, 0xE6);
    case 'd': return Color::Rgb(0x2B, 0x1E, 0x1A);
    case 'e': return Color::Rgb(0x0E, 0x0E, 0x0E);
    default:  return std::nullopt;
    }
}

Maybe HedgehogInk(const World &Shown, char Letter, int Salt)
{
    switch (Letter) {
    case 'b': return Color::Rgb(0x5C, 0x48, 0x38);
    case 's': return Color::Rgb(0xA8, 0x90, 0x70);
    case 'a': return Color::Rgb(0xD9, 0xBA, 0x9A);
    case 'd': return Color::Rgb(0x1C, 0x16, 0x12);
    case 'l': return AutumnInk(Shown.Chosen, Salt);
    default:  return std::nullopt;
    }
}

Maybe HareInk(const World &Shown, char Letter)
{
    Color Coat = Mixed(Color::Rgb(0x8E, 0x6C, 0x4A), Color::Rgb(0xF0, 0xF0, 0xEE), Weighted(Shown.Weather.Ground.Snow * 0.95f));
    switch (Letter) {
    case 'b': return Coat;
    case 'w': return Color::Rgb(0xF4, 0xF4, 0xF0);
    case 'd': return Color::Rgb(0x34, 0x28, 0x20);
    case 'a': return Mixed(Color::Rgb(0xD8, 0xA8, 0xA8), Coat, 60);
    case 'e': return Color::Rgb(0x14, 0x14, 0x14);
    default:  return std::nullopt;
    }
}

Maybe DeerInk(const World &Shown, const Animal &Beast, char Letter)
{
    float Winter = Shown.Weather.Ground.Snow;
    switch (Letter) {
    case 'b': return Mixed(Color::Rgb(0xA0, 0x6A, 0x3A), Color::Rgb(0x7C, 0x6A, 0x5A), Weighted(Winter * 0.8f));
    case 's': return Color::Rgb(0xDA, 0xBA, 0x92);
    case 'w': return Color::Rgb(0xF2, 0xEA, 0xDF);
    case 'd': return Color::Rgb(0x2A, 0x20, 0x18);
    case 'e': return Color::Rgb(0x10, 0x10, 0x10);
    case 'a':
        if (Beast.Variant != 0 || Winter > 0.5f) return std::nullopt;
        return Color::Rgb(0xD8, 0xC8, 0xA0);
    default:  return std::nullopt;
    }
}

Maybe BirdInk(const Animal &Beast, char Letter)
{
    struct Plumage {
        Color Back, Front, Belly, Beak;
    };
    static const Plumage Kinds[4] = {
        {Color::Rgb(0x7A, 0x5A, 0x3A), Color::Rgb(0xE0, 0x60, 0x2A), Color::Rgb(0xE8, 0xD8, 0xC0), Color::Rgb(0xC8, 0xA0, 0x40)},
        {Color::Rgb(0x4A, 0x78, 0xB8), Color::Rgb(0xF0, 0xD0, 0x40), Color::Rgb(0xF0, 0xF0, 0xE8), Color::Rgb(0x3A, 0x3A, 0x3A)},
        {Color::Rgb(0x8A, 0x6A, 0x4A), Color::Rgb(0xB8, 0x98, 0x68), Color::Rgb(0xD8, 0xCD, 0xB8), Color::Rgb(0x60, 0x50, 0x40)},
        {Color::Rgb(0x6A, 0x62, 0x4A), Color::Rgb(0xE8, 0xC8, 0x40), Color::Rgb(0xE8, 0xE4, 0xC0), Color::Rgb(0xD8, 0x90, 0x70)}};
    const Plumage &Coat = Kinds[Beast.Variant & 3];
    switch (Letter) {
    case 'b': return Coat.Back;
    case 'a': return Coat.Front;
    case 's': return Coat.Belly;
    case 'k': return Coat.Beak;
    case 'd': return Color::Rgb(0x5A, 0x4A, 0x3A);
    case 'e': return Color::Rgb(0x10, 0x10, 0x10);
    default:  return std::nullopt;
    }
}

Maybe OwlInk(char Letter)
{
    switch (Letter) {
    case 'b': return Color::Rgb(0x7E, 0x60, 0x40);
    case 's': return Color::Rgb(0xCD, 0xB3, 0x8E);
    case 'y': return Color::Rgb(0xEB, 0xC5, 0x2E);
    case 'e':
    case 'd': return Color::Rgb(0x18, 0x12, 0x10);
    case 'k': return Color::Rgb(0xD9, 0xA4, 0x41);
    default:  return std::nullopt;
    }
}

Maybe InkOf(const World &Shown, const Animal &Beast, char Letter, int Salt)
{
    switch (Beast.What) {
    case Kind::Fox:       return FoxInk(Letter);
    case Kind::Hedgehog:  return HedgehogInk(Shown, Letter, Salt);
    case Kind::Hare:      return HareInk(Shown, Letter);
    case Kind::Deer:      return DeerInk(Shown, Beast, Letter);
    case Kind::Bird:      return BirdInk(Beast, Letter);
    case Kind::Owl:       return OwlInk(Letter);
    case Kind::Bat:       return Letter == 'd' ? Maybe(Color::Rgb(0x24, 0x24, 0x2E)) : Letter == 'b' ? Maybe(Color::Rgb(0x3A, 0x3A, 0x46)) : std::nullopt;
    case Kind::Butterfly: {
        static const Color Wings[4] = {Color::Rgb(0xF2, 0x9A, 0x2A), Color::Rgb(0xF4, 0xE6, 0x4A), Color::Rgb(0xF2, 0xF2, 0xF0),
                                       Color::Rgb(0x6E, 0xA6, 0xEC)};
        return Letter == 'w' ? Maybe(Wings[Beast.Variant & 3]) : Letter == 'd' ? Maybe(Color::Rgb(0x2A, 0x22, 0x20)) : std::nullopt;
    }
    case Kind::Firefly:   return std::nullopt;
    }
    return std::nullopt;
}

// Lit like the rest of the world, but never quite lost in the dark.
Color Seen(const World &Shown, Color Ink)
{
    return Mixed(Lit(Shown, Ink), Ink, 36);
}

void PaintLegs(const World &Shown, Canvas &Picture, const Animal &Beast, const Pose &Pose, int Left, int HipRow)
{
    const Sprite &Body = *Pose.Body;
    const Legs   &Gait = *Pose.Gait;
    int           Wide = Body.Width();
    const int     Hips[4]   = {Gait.Hind, Gait.Hind + 2, Gait.Front - 2, Gait.Front};
    const float   Phases[4] = {0.0f, 0.5f, 0.0f, 0.5f};
    Color         Near = InkOf(Shown, Beast, 'd', 0).value_or(Color::Rgb(0x20, 0x18, 0x14));
    for (int Leg = 0; Leg < 4; Leg++) {
        int   Along = Beast.FacesLeft ? Wide - 1 - Hips[Leg] : Hips[Leg];
        float Turn  = Beast.Stride + Phases[Leg];
        int   Swing = Pose.Moving ? static_cast<int>(std::lround(std::sin(2.0f * Pi * Turn) * static_cast<float>(Gait.Reach) * 0.7f)) : 0;
        int   Lift  = Pose.Moving && std::cos(2.0f * Pi * Turn) > 0.3f ? 1 : 0;
        Color Ink   = Leg == 0 || Leg == 3 ? Near : Darker(Near, 40);
        for (int Down = 0; Down < Gait.Reach - Lift; Down++) {
            int Slide = Gait.Reach > 1 ? Swing * Down / (Gait.Reach - 1) : 0;
            Picture.Put(Left + Along + Slide, HipRow + 1 + Down, Seen(Shown, Ink));
        }
    }
}

void PaintBody(const World &Shown, Canvas &Picture, const Animal &Beast)
{
    Pose          Shape = PoseOf(Beast);
    const Sprite &Body  = *Shape.Body;
    int           Wide  = Body.Width();
    int           Left  = static_cast<int>(std::lround(Beast.X)) - Wide / 2;
    int           Top   = static_cast<int>(std::lround(Beast.Y)) - (Shape.Gait ? Body.Hip + Shape.Gait->Reach : Body.Height - 1) -
              static_cast<int>(std::lround(Beast.Rise));
    for (int Row = 0; Row < Body.Height; Row++) {
        for (int Col = 0; Col < Wide; Col++) {
            char Letter = Body.Rows[Row][Beast.FacesLeft ? Wide - 1 - Col : Col];
            if (Letter == '.') continue;
            if (Maybe Ink = InkOf(Shown, Beast, Letter, Row * 7 + Col)) Picture.Put(Left + Col, Top + Row, Seen(Shown, *Ink));
        }
    }
    if (Shape.Gait) PaintLegs(Shown, Picture, Beast, Shape, Left, Top + Body.Hip);
}

// A bat or a butterfly: faded in and out by the hour.
void PaintWinged(const World &Shown, Canvas &Picture, const Animal &Beast)
{
    Pose          Shape = PoseOf(Beast);
    const Sprite &Body  = *Shape.Body;
    int           Wide  = Body.Width();
    int           Left  = static_cast<int>(std::lround(Beast.X)) - Wide / 2;
    int           Top   = static_cast<int>(std::lround(Beast.Y)) - Body.Height / 2;
    for (int Row = 0; Row < Body.Height; Row++) {
        for (int Col = 0; Col < Wide; Col++) {
            char Letter = Body.Rows[Row][Beast.FacesLeft ? Wide - 1 - Col : Col];
            if (Letter == '.') continue;
            if (Maybe Ink = InkOf(Shown, Beast, Letter, 0))
                Picture.Blend(Left + Col, Top + Row, Seen(Shown, *Ink), Weighted(Beast.Presence));
        }
    }
}

void PaintFirefly(const World &Shown, Canvas &Picture, const Animal &Beast)
{
    static const Color Glow = Color::Rgb(0xD0, 0xF0, 0x58);
    float Blink = 0.5f + 0.7f * Wave(Shown.Weather.Clock * (0.28f + 0.2f * Beast.Phase) + Beast.Phase * 5.0f);
    Blink       = std::clamp(Blink, 0.0f, 1.0f);
    float Shine = Blink * Blink * Beast.Presence;
    if (Shine < 0.03f) return;
    int X = static_cast<int>(std::lround(Beast.X)), Y = static_cast<int>(std::lround(Beast.Y));
    Picture.Blend(X, Y, Glow, Weighted(Shine));
    for (int Dx = -1; Dx <= 1; Dx++)
        for (int Dy = -1; Dy <= 1; Dy++)
            if (Dx != 0 || Dy != 0) Picture.Blend(X + Dx, Y + Dy, Glow, Weighted(Shine * (Dx != 0 && Dy != 0 ? 0.12f : 0.32f)));
}

}  // namespace

bool OnGround(const Animal &Beast)
{
    switch (Beast.What) {
    case Kind::Fox:
    case Kind::Hedgehog:
    case Kind::Hare:
    case Kind::Deer: return Beast.Doing != Act::Away;
    case Kind::Bird: return Beast.Doing == Act::Peck;
    default:         return false;
    }
}

void PaintAnimal(const World &Shown, Canvas &Picture, const Animal &Beast)
{
    if (Drifts(Beast.What)) {
        if (Beast.Presence < 0.02f) return;
        if (Beast.What == Kind::Firefly) PaintFirefly(Shown, Picture, Beast);
        else PaintWinged(Shown, Picture, Beast);
        return;
    }
    if (Beast.Doing == Act::Away) return;
    PaintBody(Shown, Picture, Beast);
}

}  // namespace forest
